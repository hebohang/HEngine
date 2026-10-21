"""Install pinned SDK packages with explicit, byte-oriented license input."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--accept-licenses', action='store_true', required=True)
    args = parser.parse_args()
    pin = json.loads(Path(__file__).with_name('toolchain.lock.json').read_text(encoding='utf-8-sig'))
    tools = args.root / 'sdk' / 'cmdline-tools' / pin['commandLineTools']['version']
    command = [str(args.java_home / 'bin/java.exe'),
               '-Dcom.android.sdklib.toolsdir=' + str(tools), '-classpath',
               str(tools / 'lib/sdkmanager-classpath.jar'),
               'com.android.sdklib.tool.sdkmanager.SdkManagerCli',
               '--sdk_root=' + str(args.sdk), 'platform-tools',
               'platforms;' + pin['platform'], 'build-tools;' + pin['buildTools']]
    result = subprocess.run(command, input=b'y\n' * 100, timeout=1800)
    if result.returncode:
        raise SystemExit(result.returncode)
    for relative in [f"platforms/{pin['platform']}/android.jar",
                     f"build-tools/{pin['buildTools']}/aapt2.exe", 'platform-tools/adb.exe']:
        if not (args.sdk / relative).is_file():
            raise SystemExit(f'SDK installation is incomplete: {relative}')


if __name__ == '__main__':
    main()
