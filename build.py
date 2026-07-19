#!/usr/bin/env python3
"""AES67 项目统一构建脚本 — 从 Git Bash 直接调，不依赖用户双击"""
import subprocess, sys, os
# 强制终端输出 UTF-8，避免 GBK 乱码
if sys.stdout.encoding != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

VSDIR = r"D:\Program Files\Microsoft Visual Studio\18\Community"
VCVARS = f'{VSDIR}\\VC\\Auxiliary\\Build\\vcvars64.bat'
WDK_BIN = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0"
DRIVER_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'driver')
DRIVER_PROJ = f'{DRIVER_DIR}\\AES67Driver.vcxproj'
DRIVER_OUT = f'{DRIVER_DIR}\\x64\\Win10'

def run_cmd(title, cmds):
    """写临时 .bat 文件再执行，避免路径转义问题"""
    bat = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_build_tmp.bat')
    # WDK 环境变量——msbuild 需要它们找到 portcls.h 等内核头文件
    wdk_root = r'C:\Program Files (x86)\Windows Kits\10'
    script = f'@echo off\r\n'
    script += f'set WDKCONTENTROOT={wdk_root}\r\n'
    script += f'set WindowsSdkDir={wdk_root}\\\r\n'
    script += f'set WindowsSDKVersion=10.0.28000.0\r\n'
    script += f'set WindowsLibPath={wdk_root}\\Lib\\10.0.28000.0\\km\\x64\r\n'
    script += f'call "{VCVARS}"\r\n'
    script += f'cd /d "{DRIVER_DIR}"\r\n'
    script += '\r\n'.join(cmds)
    with open(bat, 'w', encoding='ascii') as f:
        f.write(script)
    print(f"\n=== {title} ===")
    r = subprocess.run(['cmd', '/c', bat], capture_output=True, timeout=120)
    os.remove(bat)
    # 输出写入日志文件避免 GBK 终端乱码
    log = os.path.join(DRIVER_DIR, '_build.log')
    with open(log, 'wb') as f:
        if r.stdout: f.write(r.stdout)
        if r.stderr: f.write(r.stderr)
    if r.returncode == 0:
        print(f"  BUILD SUCCESS (log: {log})")
    else:
        print(f"  BUILD FAILED rc={r.returncode} (log: {log})")
        # 打印最后几行
        with open(log, 'r', encoding='gbk', errors='replace') as f:
            lines = f.readlines()
            for l in lines[-8:]: print(f"  {l.rstrip().encode('utf-8', errors='replace').decode('utf-8', errors='replace')}")
        sys.exit(r.returncode)

def build_driver():
    run_cmd("Building AES67 Driver", [
        f'msbuild {DRIVER_PROJ} /p:Configuration=Win10 /p:Platform=x64 /t:Build'
    ])

def sign_driver():
    inf2cat = f'{WDK_BIN}\\x86\\inf2cat.exe'
    signtool = f'{WDK_BIN}\\x64\\signtool.exe'
    run_cmd("Signing Driver INF", [
        f'cd /d "{DRIVER_OUT}"',
        f'"{inf2cat}" /driver:. /os:10_19H1_X64',
        f'"{signtool}" sign /fd SHA256 /a aes67driver.cat'
    ])

def build_ioctl_test():
    """编译 IOCTL 测试程序"""
    run_cmd("Building IOCTL Test", [
        f'cd /d "{os.path.dirname(os.path.abspath(__file__))}"',
        'cl /EHsc /W3 /nologo ioctl_test.cpp /Fe:ioctl_test.exe /I. setupapi.lib'
    ])

def run_ioctl_test():
    """运行 IOCTL 测试程序"""
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ioctl_test.exe')
    if not os.path.exists(exe):
        print("ioctl_test.exe not found, building first...")
        build_ioctl_test()
    r = subprocess.run([exe], capture_output=True, timeout=10)
    print((r.stdout or b'').decode('gbk', errors='replace'))
    if r.stderr:
        print((r.stderr or b'').decode('gbk', errors='replace'))
    print(f"RC: {r.returncode}")

def _uninstall_all_aes67_oem_packages():
    """动态枚举并删除所有匹配 AES67Driver 的 oem*.inf 驱动包。

    之前硬编码 oem175/oem176 是错的——每台机器、每次安装的 oem 编号都不同,
    硬编码几乎必然删不掉真正的旧包, 导致 /add-driver 只是叠加安装, 旧的
    KS interface 类注册残留, 改了 INF 的 AddInterface 也不生效(端点状态不变)。
    这里用 pnputil /enum-drivers 找出所有 OriginalName=AES67Driver.inf 的包,
    逐个 /delete-driver /force, 保证下一次 /install 是干净的全新注册。
    """
    try:
        r = subprocess.run('pnputil /enum-drivers', shell=True,
                            capture_output=True, timeout=30)
        out = (r.stdout or b'').decode('gbk', errors='replace')
    except Exception as e:
        print(f"enum-drivers failed: {e}")
        return
    # 解析出 "Published Name: oemNN.inf" 且其 Original Name 为 AES67Driver.inf 的条目
    oem = None
    targets = []
    for line in out.splitlines():
        s = line.strip()
        low = s.lower()
        if low.startswith('published name') or low.startswith('发布名称'):
            oem = s.split(':', 1)[1].strip() if ':' in s else None
        elif (low.startswith('original name') or low.startswith('原始名称')) and oem:
            if 'aes67driver.inf' in low:
                targets.append(oem)
            oem = None
    if not targets:
        print("No existing AES67Driver oem package found (clean).")
        return
    print(f"Found existing AES67 driver packages: {targets}")
    for pkg in targets:
        run_cmd(f"Deleting old package {pkg}", [
            f'pnputil /delete-driver {pkg} /uninstall /force'
        ])

def install_driver():
    # 1. 先移除设备节点
    run_cmd("Removing old device node", [
        'devcon remove *AES67Driver 2>nul',
        'pnputil /remove-device /deviceid *AES67Driver 2>nul',
        'ping -n 3 127.0.0.1 >nul'
    ])
    # 2. 动态删除所有旧的 AES67 驱动包(修复硬编码 oem175/176 的老 bug)
    _uninstall_all_aes67_oem_packages()
    # 3. 全新安装(此时 INF 的 KS interface 注册会重建)
    run_cmd("Installing new driver", [
        f'pnputil /add-driver {DRIVER_OUT}\\AES67Driver.inf /install'
    ])

def build_engine():
    """编译用户态音频引擎 (M4 Framework + standalone tests)"""
    proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'engine')
    run_cmd("Building Audio Engine (M5 AES67 Tx)", [
        f'cd /d "{proj}"',
        # M5 Engine with AES67 transmit
        'cl /EHsc /std:c++17 /O2 /nologo /W3 /utf-8 '
        'main.cpp aes67_engine.cpp wasapi_device.cpp audio_thread.cpp logger.cpp '
        'network_thread.cpp sap_announcer.cpp '
        'network_receiver.cpp audio_render_thread.cpp '
        'ptp_clock.cpp ptp_thread.cpp '
        'pipe_server.cpp '
        '/Fe:aes67_engine.exe ole32.lib avrt.lib ws2_32.lib winmm.lib',
        # Standalone smoke tests (unchanged)
        'cl /EHsc /std:c++17 /O2 /nologo /W3 /utf-8 loopback.cpp /Fe:loopback.exe ole32.lib avrt.lib',
        'cl /EHsc /std:c++17 /O2 /nologo /W3 /utf-8 sine_test.cpp /Fe:sine_test.exe ole32.lib avrt.lib',
        # IPC pipe test
        'cl /EHsc /std:c++17 /O2 /nologo /W3 /utf-8 _pipetest.cpp /Fe:_pipetest.exe'
    ])

def build_userland():
    build_engine()

def build_asio():
    """编译 ASIO DLL"""
    asio_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'asio')
    run_cmd("Building ASIO DLL (M8)", [
        f'cd /d "{asio_dir}"',
        # Minimal ASIO DLL (known working)
        'cl /LD /EHsc /std:c++17 /O2 /nologo /W3 /utf-8 /MT '
        'asio_minimal.cpp '
        '/Fe:AES67_ASIO.dll ws2_32.lib winmm.lib '
        '/link /DEF:asio_exports.def',
        # Quick load test
        'cl /EHsc /std:c++17 /nologo /W3 /utf-8 _test_load.cpp /Fe:_test_load.exe'
    ])

def build_panel():
    """编译 Qt 配置面板"""
    panel_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'panel')
    qt_prefix = r"D:\Qt\6.8.3\msvc2022_64"
    build_dir = os.path.join(panel_dir, 'build')

    run_cmd("Building Qt Panel (M9)", [
        f'if not exist "{build_dir}" mkdir "{build_dir}"',
        f'cd /d "{build_dir}"',
        f'cmake -S "{panel_dir}" -B "{build_dir}" '
        f'-DCMAKE_PREFIX_PATH="{qt_prefix}" '
        f'-G "Visual Studio 18 2026" -A x64',
        f'cmake --build "{build_dir}" --config Release'
    ])

def help_text():
    print("Usage: python build.py [driver|sign|install|user|panel|all]")
    print("  driver   - Build WDM driver")
    print("  sign     - Sign driver INF + catalog")
    print("  install  - Install signed driver")
    print("  user     - Build user-mode AES67 engine")
    print("  panel    - Build Qt control panel")
    print("  all      - Build + sign + install (full cycle)")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        help_text()
        sys.exit(1)
    cmd = sys.argv[1]
    {'driver': build_driver, 'sign': sign_driver, 'install': install_driver,
     'user': build_userland, 'ioctl': build_ioctl_test, 'test-ioctl': run_ioctl_test,
     'asio': build_asio, 'panel': build_panel,
     'all': lambda: (build_driver(), sign_driver(), install_driver()),
     'help': help_text}.get(cmd, help_text)()
