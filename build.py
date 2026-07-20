#!/usr/bin/env python3
"""AES67 项目统一构建脚本 — 从 Git Bash / CMD 直接调，不依赖用户双击。

  环境变量（全部可选，不存在时用默认值）：
    VS_DIR          Visual Studio 安装目录
    WDK_DIR         Windows Kits 安装目录
    WDK_VERSION     WDK/SDK 版本号
    QT_DIR          Qt 安装目录（含 msvc2022_64 子目录）
"""

import subprocess, sys, os, re, datetime

# ── 编码 ────────────────────────────────────────────────
if sys.stdout.encoding != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# ── 路径推导（优先环境变量，不存在时用默认值） ────────
ROOT = os.path.dirname(os.path.abspath(__file__))
DRIVER_DIR = os.path.join(ROOT, 'driver')
DRIVER_OUT = os.path.join(DRIVER_DIR, 'x64', 'Win10')

VSDIR = os.environ.get('VS_DIR', r'D:\Program Files\Microsoft Visual Studio\18\Community')
VCVARS = os.path.join(VSDIR, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
WINSDK_VERSION = os.environ.get('WDK_VERSION', '10.0.28000.0')
WINSDK_DIR = os.environ.get('WDK_DIR', r'C:\Program Files (x86)\Windows Kits\10')
WINSDK_BIN = os.path.join(WINSDK_DIR, 'bin', WINSDK_VERSION)

# ── 核心：运行批处理命令 ────────────────────────────────
def run_cmd(title, cmds):
    """写临时 .bat 文件，用 cmd /c 执行，输出记入日志"""
    bat = os.path.join(ROOT, '_build_tmp.bat')
    log = os.path.join(DRIVER_DIR, '_build.log')

    script = '@echo off\r\n'
    script += f'set "WDKCONTENTROOT={WINSDK_DIR}"\r\n'
    script += f'set "WindowsSdkDir={WINSDK_DIR}"\r\n'
    script += f'set "WindowsSdkVersion={WINSDK_VERSION}"\r\n'
    script += f'call "{VCVARS}" >nul 2>&1\r\n'
    script += f'cd /d "{DRIVER_DIR}"\r\n'
    script += '\r\n'.join(cmds)

    with open(bat, 'w', encoding='ascii') as f:
        f.write(script)

    print(f"\n=== {title} === (log: {log})")
    try:
        with open(log, 'wb') as f:
            r = subprocess.run(['cmd', '/c', bat],
                              stdin=subprocess.DEVNULL,
                              stdout=f, stderr=subprocess.STDOUT, timeout=120)
    finally:
        try: os.remove(bat)
        except OSError: pass

    if r.returncode == 0:
        print(f"  BUILD SUCCESS")
    else:
        print(f"  BUILD FAILED rc={r.returncode}")
        with open(log, 'r', encoding='gbk', errors='replace') as f:
            lines = f.readlines()
            for l in lines[-8:]:
                print(f"  {l.rstrip()}")
        sys.exit(r.returncode)

# ── 驱动 ─────────────────────────────────────────────────
def build_driver():
    run_cmd("Building AES67 Driver", [
        f'msbuild {DRIVER_DIR}\\AES67Driver.vcxproj '
        f'/p:Configuration=Win10 /p:Platform=x64 /t:Build'
    ])

def _fix_driverver_utc():
    """把输出目录及子目录 INF 的 DriverVer 日期改成 UTC 前一天，避免 inf2cat 拒签。

    WORKAROUND: 标准做法应通过 vcxproj 的 StampInf 元数据控制日期，
    但 WDK StampInf 任务不支持 UTC 模式。日期偏移一天不影响驱动功能，
    仅影响 INF 版本排序（开发阶段可接受）。正式发布用 WHQL 不需要 inf2cat。"""
    safe = (datetime.datetime.now(datetime.timezone.utc).date()
            - datetime.timedelta(days=1)).strftime('%m/%d/%Y')
    for inf in [f'{DRIVER_OUT}\\AES67Driver.inf',
                f'{DRIVER_OUT}\\AES67Driver\\AES67Driver.inf']:
        if not os.path.exists(inf): continue
        with open(inf, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
        new_text, n = re.subn(r'(DriverVer\s*=\s*)\d{2}/\d{2}/\d{4}',
                              r'\g<1>' + safe, text, count=1)
        if n:
            with open(inf, 'w', encoding='utf-8') as f:
                f.write(new_text)
            print(f"  DriverVer → {safe}  ({os.path.basename(os.path.dirname(inf))}\\{os.path.basename(inf)})")

def sign_driver():
    _fix_driverver_utc()
    inf2cat = f'{WINSDK_BIN}\\x86\\inf2cat.exe'
    signtool = f'{WINSDK_BIN}\\x64\\signtool.exe'
    run_cmd("Signing Driver INF", [
        f'cd /d "{DRIVER_OUT}"',
        f'"{inf2cat}" /driver:. /os:10_19H1_X64',
        f'"{signtool}" sign /fd SHA256 /a aes67driver.cat'
    ])

def _uninstall_all_aes67_oem_packages():
    """枚举并删除所有 AES67Driver.inf 对应的 oem*.inf 包"""
    try:
        r = subprocess.run('pnputil /enum-drivers', shell=True,
                           stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=30)
        out = (r.stdout or b'').decode('gbk', errors='replace')
    except Exception as e:
        print(f"  enum-drivers failed: {e}")
        return
    targets = []
    pending_oem = None
    for line in out.splitlines():
        line = line.strip()
        lo = line.lower()
        if lo.startswith('published name') or lo.startswith('发布名称'):
            pending_oem = line.split(':', 1)[1].strip() if ':' in line else None
        elif pending_oem and (lo.startswith('original name') or lo.startswith('原始名称')):
            if 'aes67driver.inf' in lo:
                targets.append(pending_oem)
            pending_oem = None
    if not targets:
        print("  No existing AES67Driver oem package found.")
        return
    print(f"  Found old packages: {targets}")
    for pkg in targets:
        run_cmd(f"Deleting {pkg}", [
            f'pnputil /delete-driver {pkg} /uninstall /force'
        ])

def install_driver():
    devcon = f'{WINSDK_BIN}\\..\\..\\Tools\\{WINSDK_VERSION}\\x64\\devcon.exe'
    run_cmd("Removing old device node", [
        f'"{devcon}" remove *AES67Driver 2>nul',
        'pnputil /remove-device /deviceid *AES67Driver 2>nul',
        'ping -n 3 127.0.0.1 >nul'
    ])
    _uninstall_all_aes67_oem_packages()
    run_cmd("Installing new driver", [
        f'pnputil /add-driver {DRIVER_OUT}\\AES67Driver.inf /install'
    ])

# ── 用户态引擎 ──────────────────────────────────────────
ENGINE_SRCS = [
    'main.cpp', 'aes67_engine.cpp', 'wasapi_device.cpp', 'audio_thread.cpp',
    'logger.cpp', 'network_thread.cpp', 'sap_announcer.cpp',
    'network_receiver.cpp', 'audio_render_thread.cpp',
    'ptp_clock.cpp', 'ptp_thread.cpp', 'pipe_server.cpp'
]
ENGINE_LIBS = 'ole32.lib avrt.lib ws2_32.lib winmm.lib'
ENGINE_FLAGS = '/EHsc /std:c++17 /O2 /nologo /W3 /utf-8'

def build_engine():
    proj = os.path.join(ROOT, 'engine')
    cmds = [f'cd /d "{proj}"']
    cmds.append(f'cl {ENGINE_FLAGS} {" ".join(ENGINE_SRCS)} '
                f'/Fe:aes67_engine.exe {ENGINE_LIBS}')
    cmds.append(f'cl {ENGINE_FLAGS} loopback.cpp /Fe:loopback.exe ole32.lib avrt.lib')
    cmds.append(f'cl {ENGINE_FLAGS} sine_test.cpp /Fe:sine_test.exe ole32.lib avrt.lib')
    cmds.append(f'cl {ENGINE_FLAGS} _pipetest.cpp /Fe:_pipetest.exe')
    run_cmd("Building Audio Engine (M5 AES67 Tx)", cmds)

def build_userland():
    build_engine()

# ── ASIO ─────────────────────────────────────────────────
def build_asio():
    asio_dir = os.path.join(ROOT, 'asio')
    run_cmd("Building ASIO DLL (M8)", [
        f'cd /d "{asio_dir}"',
        f'cl /LD {ENGINE_FLAGS} /MD '
        f'asio_minimal.cpp /Fe:AES67_ASIO.dll ws2_32.lib winmm.lib '
        f'/link /DEF:asio_exports.def',
        f'cl {ENGINE_FLAGS} _test_load.cpp /Fe:_test_load.exe'
    ])

# ── Qt 面板 ──────────────────────────────────────────────
def build_panel():
    panel_dir = os.path.join(ROOT, 'panel')
    qt_prefix = os.environ.get('QT_DIR', r'D:\Qt\6.8.3\msvc2022_64')
    build_dir = os.path.join(panel_dir, 'build')

    # 自动检测 cmake generator（先查当前 VS 版本的 generator 名）
    vs_year = os.path.basename(VSDIR)  # e.g. "18" → 2026, "2022" → 2022
    gen = f"Visual Studio {vs_year}"
    if vs_year.isdigit() and len(vs_year) == 2:
        gen = f"Visual Studio {2000 + int(vs_year)}"

    run_cmd("Building Qt Panel (M9)", [
        f'if not exist "{build_dir}" mkdir "{build_dir}"',
        f'cd /d "{build_dir}"',
        f'cmake -S "{panel_dir}" -B "{build_dir}" '
        f'-DCMAKE_PREFIX_PATH="{qt_prefix}" '
        f'-G "{gen}" -A x64',
        f'cmake --build "{build_dir}" --config Release'
    ])

# ── IOCTL 测试 ───────────────────────────────────────────
def build_ioctl_test():
    run_cmd("Building IOCTL Test", [
        f'cd /d "{ROOT}"',
        f'cl /EHsc /W3 /nologo ioctl_test.cpp /Fe:ioctl_test.exe '
        f'/I. setupapi.lib'
    ])

def run_ioctl_test():
    exe = os.path.join(ROOT, 'ioctl_test.exe')
    if not os.path.exists(exe):
        print("ioctl_test.exe not found, building first...")
        build_ioctl_test()
    r = subprocess.run([exe], stdin=subprocess.DEVNULL,
                       capture_output=True, timeout=10)
    print((r.stdout or b'').decode('gbk', errors='replace'))
    if r.stderr:
        print((r.stderr or b'').decode('gbk', errors='replace'))
    print(f"RC: {r.returncode}")

# ── 帮助 ─────────────────────────────────────────────────
def help_text():
    print("Usage: python build.py <command>")
    print("Commands:")
    for c, d in COMMANDS.items():
        print(f"  {c:<12} {d}")
    print()
    print("Environment variables (optional, fallback to defaults):")
    print("  VS_DIR         Visual Studio install dir")
    print("  WDK_DIR        Windows Kits install dir")
    print("  WDK_VERSION    WDK/SDK version, e.g. 10.0.28000.0")
    print("  QT_DIR         Qt dir (must contain msvc2022_64 subdir)")

COMMANDS = {
    'driver':     'Build WDM driver',
    'sign':       'Sign driver INF + catalog',
    'install':    'Install signed driver',
    'user':       'Build user-mode AES67 engine',
    'panel':      'Build Qt control panel',
    'asio':       'Build ASIO DLL',
    'ioctl':      'Build IOCTL test program',
    'test-ioctl': 'Run IOCTL test (auto-builds if needed)',
    'all':        'Build + sign + install (full cycle)',
    'help':       'Show this help',
}

# ── 入口 ─────────────────────────────────────────────────
if __name__ == '__main__':
    if len(sys.argv) < 2:
        help_text()
        sys.exit(1)
    cmd = sys.argv[1]
    actions = {
        'driver': build_driver, 'sign': sign_driver, 'install': install_driver,
        'user': build_userland, 'panel': build_panel, 'asio': build_asio,
        'ioctl': build_ioctl_test, 'test-ioctl': run_ioctl_test,
        'all': lambda: (build_driver(), sign_driver(), install_driver()),
        'help': help_text,
    }
    actions.get(cmd, help_text)()
