# InputLeafPlus

InputLeafPlus는 한 대의 키보드와 마우스로 여러 컴퓨터를 제어하는 오픈 소스 소프트웨어입니다. [Input Leap](https://github.com/input-leap/input-leap)을 기반으로 하며, 특히 Windows와 macOS를 함께 사용할 때 필요한 입력 변환, 클립보드 안정성, 빌드 편의성을 보강했습니다.

이 저장소의 개선 사항은 원본 프로젝트의 `master` 기준 커밋 `34a34fb2`와 비교해 정리했습니다.

## 원본 대비 개선 사항

### Windows와 macOS 사이의 키보드 입력

- 화면별 키 매핑 설정 창을 추가했습니다. 서버의 키를 각 클라이언트에서 어떤 키로 보낼지 GUI에서 지정할 수 있습니다.
- Shift, Ctrl, Alt, Meta, Super 키를 화면별로 다시 배치할 수 있습니다.
- Windows 키보드를 Mac 배열에 맞추는 프리셋을 제공합니다. `Alt`는 `Command`로, `Windows` 키는 `Option`으로 보냅니다. 스페이스바 옆의 키가 Mac에서도 스페이스바 옆의 키로 동작합니다.
- 저장된 modifier 설정을 불러올 때 임의로 덮어쓰던 자동 마이그레이션을 제거했습니다. 사용자가 지정한 배치가 그대로 유지됩니다.
- Caps Lock을 F18로 바꾸는 프리셋을 제공합니다.
- Caps Lock을 다른 키로 매핑해 원격 화면에서 사용할 때 Windows 로컬 Caps Lock 상태가 함께 바뀌지 않도록 처리했습니다.
- 키 누름, 키 뗌, 키 반복, 키보드 브로드캐스트에 동일한 화면별 매핑을 적용합니다.
- 매핑된 보조 키의 원래 modifier 상태가 함께 전달되어 조합키로 오인되는 문제를 방지했습니다.
- Windows 한/영 키의 가상 키 코드 매핑을 바로잡았습니다.
- macOS에서 F17~F20 확장 기능 키를 인식하도록 추가했습니다.
- Mac을 서버로 사용할 때 지정한 USB 키보드 입력을 원격 화면으로 보내지 않고 Mac에서만 사용할 수 있습니다.
- Mac 키보드 상단 기능 키를 Windows에서 F1~F12로 보내는 설정을 제공합니다.

Mac 서버의 `설정`에서 `Mac 서버 입력` 항목을 사용하면 됩니다. USB 키보드 선택이나 기능 키 설정을 바꾸면 실행 중인 서버에 바로 반영됩니다.

### 화면별 스크롤 설정

- 클라이언트 화면마다 스크롤 방향을 반대로 설정할 수 있습니다.
- 이전 개발 버전의 `reverseMouse` 설정은 `reverseScroll`로 자동 호환됩니다.

### 클립보드 공유 안정성

- **Windows에서 복사한 내용이 macOS로 전달되지 않던 문제를 해결했습니다.** Windows 서비스로 동작하는 데몬이 서버 프로세스를 항상 `SYSTEM` 권한으로 실행하고 있었고, `SYSTEM` 프로세스는 로그인한 사용자의 클립보드를 읽을 수 없었습니다. 자세한 내용은 아래 [Windows 서버 권한](#windows-서버-권한) 항목을 참고하세요.
- macOS의 `public.utf8-plain-text` 형식을 지원해 한글을 포함한 UTF-8 텍스트 전달을 보강했습니다.
- Windows와 macOS 사이에서 텍스트 줄바꿈을 각 운영체제 형식에 맞게 변환합니다.
- 화면을 전환하는 순간 서버 클립보드를 즉시 읽어 최신 내용이 빠지는 문제를 줄였습니다.
- 클립보드 소유권 알림 순서가 엇갈릴 때 발생하던 잘못된 업데이트와 충돌 가능성을 방어했습니다.
- macOS 클립보드 쓰기 결과를 실제 성공 여부대로 반환하고 Core Foundation 객체를 정리합니다.
- 클립보드 사용 여부와 최대 크기 옵션을 함께 안정적으로 처리하며, 값이 빠진 잘못된 옵션도 안전하게 무시합니다.
- 다른 프로그램이 Windows 클립보드를 잠시 점유하고 있으면 곧바로 실패하지 않고 재시도합니다.
- Windows 클립보드 읽기 실패를 성공으로 보고하던 문제를 바로잡았습니다.

### Windows 서버 권한

Windows에서 복사한 내용이 macOS로 전달되지 않는 문제의 원인이었습니다.

데몬(`input-leapd`)은 Windows 서비스이므로 **세션 0**에서 실행됩니다. 서버 프로세스를 띄우기 전에 "지금 로그인 화면인가"를 판별하려고 `OpenInputDesktop()`으로 데스크톱 이름을 읽었는데, 이 API는 **호출한 프로세스가 속한 윈도우 스테이션 안에서만** 동작합니다. 세션 0의 `Service-0x0-3e7$`에서 실행되는 데몬은 로그인한 사용자의 데스크톱을 원리적으로 볼 수 없어 항상 빈 문자열을 받았고, `"" != "Default"`라는 판정 때문에 **매번 로그인 화면으로 오인해 서버를 `SYSTEM` 권한으로 실행**했습니다.

`SYSTEM` 프로세스는 로그인한 사용자의 OLE 클립보드를 읽을 수 없습니다. 이 때문에 방향에 따라 증상이 갈렸습니다.

| 방향 | 동작 | 결과 |
| --- | --- | --- |
| macOS → Windows | `SetClipboardData`로 쓰기 | 정상 |
| Windows → macOS | `EnumClipboardFormats`로 읽기 | `CF_UNICODETEXT`가 보이지 않아 항상 빈 클립보드 |

데스크톱 이름 추측을 걷어내고, **사용자 토큰을 먼저 시도한 뒤 실패할 때만** `winlogon.exe`의 권한으로 되돌아가도록 바꿨습니다. `WTSQueryUserToken()`은 아무도 로그인하지 않았을 때만 실패하므로, 이것이 로그인 화면을 판정하는 정확한 방법입니다.

**알아두어야 할 트레이드오프.** 서버가 일반 사용자 권한으로 실행되므로 잠금 화면과 UAC 창은 다른 컴퓨터에서 조작할 수 없습니다. 잠금 화면 제어가 더 필요하면 GUI 설정에서 권한 상승을 `Always`로 바꾸면 되지만, 그러면 클립보드 공유가 다시 동작하지 않습니다. 둘 중 하나만 선택할 수 있습니다.

### Windows 서비스 완전 종료

- Windows 서비스는 자동 시작이 아닌 수동 시작으로 설치됩니다.
- InputLeafPlus를 실행할 때 서비스 모드라면 서비스도 함께 시작됩니다.
- 트레이 아이콘의 `종료`를 누르면 클라이언트 또는 서버 프로세스를 정상 종료한 뒤 백그라운드 서비스까지 중지합니다.
- Windows 메인 창에서도 `종료` 버튼으로 같은 종료 동작을 실행할 수 있습니다.
- 종료 요청 후 서비스가 실제로 멈췄는지 확인하며, 기존 설치의 권한이 부족하면 Windows 관리자 승인 경로로 다시 중지합니다.
- 서비스 중지에 실패하면 백그라운드 프로세스를 남긴 채 GUI만 사라지지 않도록 종료를 취소하고 오류를 알립니다.
- 기존 설치와의 업그레이드 호환성을 위해 내부 서비스 ID `InputLeap`과 실행 파일명은 유지합니다. 사용자에게 표시되는 제품명과 서비스 이름은 InputLeafPlus입니다.

### 빌드와 패키징

- Windows용 `build_windows.bat`와 `clean_build.ps1`을 추가했습니다.
- Windows 빌드에 필요한 Visual Studio C++ Build Tools, Qt 6.6.3, OpenSSL 3, Bonjour 호환 SDK를 확인하고 필요한 항목은 사용자 동의 후 준비합니다.
- 실행 중인 서비스를 안전하게 멈춘 뒤 빌드 폴더를 교체하고, 원래 실행 중이었던 경우에만 다시 시작합니다.
- macOS용 `build_macos.command`를 추가해 Homebrew 의존성 확인부터 ARM64 Release 빌드까지 한 번에 진행합니다.
- macOS 앱을 임시 스테이징 번들에서 완성한 뒤 서명 검증에 성공한 결과만 교체해, 실패한 빌드가 기존 앱을 손상하지 않도록 했습니다.
- macOS 앱과 DMG 이름, Windows 설치 파일과 바로가기 이름을 InputLeafPlus로 통일했습니다.
- CI 빌드 대상을 Windows x86-64/Qt 6과 macOS ARM64/Qt 6 중심으로 정리했습니다.

## 빌드

### Windows

저장소 루트에서 실행합니다.

```powershell
.\build_windows.bat
```

이 스크립트가 Visual Studio C++ Build Tools, Qt 6.6.3, OpenSSL 3, Bonjour 호환 SDK를 확인하고 없는 항목은 동의를 받아 준비합니다. 실행 중인 서비스가 있으면 안전하게 멈춘 뒤 빌드하고, 원래 실행 중이었던 경우에만 다시 시작합니다.

| 경로 | 내용 |
| --- | --- |
| `build\input-leap-install\` | 실행 파일 모음. 여기의 `input-leap.exe`를 실행하면 됩니다 |
| `build\bin\Release\` | 링커가 만든 원본 실행 파일 |
| `build\installer-inno\bin\` | 설치 파일. Inno Setup 6이 설치되어 있을 때만 생성됩니다 |

#### 데몬을 고쳤다면 수동으로 복사해야 합니다

Windows 서비스는 빌드 폴더가 아니라 **설치된 위치**에서 실행됩니다.

```powershell
sc qc InputLeap
# BINARY_PATH_NAME : "C:\Program Files\InputLeap\input-leapd.exe"
```

`build_windows.bat`은 `build\input-leap-install\`만 갱신하므로, 데몬 코드(`MSWindowsWatchdog`, `DaemonApp` 등)를 고친 뒤 재빌드만 하면 **서비스는 계속 예전 바이너리를 실행합니다.** 관리자 PowerShell에서 직접 교체해야 합니다.

```powershell
Stop-Service InputLeap -Force
Copy-Item "<저장소>\build\input-leap-install\input-leapd.exe" `
          "C:\Program Files\InputLeap\input-leapd.exe" -Force
Start-Service InputLeap
```

GUI(`input-leap.exe`)와 서버(`input-leaps.exe`)는 GUI를 실행한 폴더 기준으로 동작하므로 재빌드만으로 충분합니다.

#### 개별 대상만 빌드하기

전체 재빌드 없이 특정 대상만 다시 만들 수 있습니다. `cmake`는 PATH에 없을 수 있으므로 전체 경로를 씁니다.

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --target input-leap
```

주요 대상: `input-leap`(GUI), `input-leaps`(서버), `input-leapc`(클라이언트), `input-leapd`(데몬), `platform`(플랫폼 라이브러리), `unittests`, `integtests`.

#### 테스트

```powershell
.\build\bin\Release\unittests.exe
.\build\bin\Release\integtests.exe
```

클립보드만 확인하려면 `integtests.exe --gtest_filter=*Clipboard*`를 씁니다.

#### 로그 위치

| 파일 | 내용 |
| --- | --- |
| `C:\ProgramData\InputLeap\input-leapd.log` | 데몬 로그. 권한 상승과 서버 실행 여부가 여기에 남습니다 |
| GUI의 로그 창 | 서버 로그. 클립보드 전송 내역을 볼 수 있습니다 |

서버가 어떤 계정으로 실행 중인지 확인하려면 다음을 실행합니다. 정상이라면 `SYSTEM`이 아니라 로그인한 사용자 이름이 나와야 합니다.

```powershell
Get-CimInstance Win32_Process -Filter "Name='input-leaps.exe'" |
    ForEach-Object { (Invoke-CimMethod -InputObject $_ -MethodName GetOwner).User }
```

### macOS Apple Silicon

```bash
chmod +x build_macos.command
./build_macos.command
```

Homebrew 의존성 확인부터 ARM64 Release 빌드까지 한 번에 진행합니다. 임시 스테이징 번들에서 앱을 완성한 뒤 서명 검증에 성공한 결과만 교체하므로, 빌드가 실패해도 기존 앱은 손상되지 않습니다.

완성된 앱은 `build/bundle/InputLeafPlus.app`에 생성됩니다.

## 호환성과 라이선스

설정 파일 형식, 네트워크 프로토콜, `input-leap`, `input-leaps`, `input-leapc`, `input-leapd` 실행 파일명은 원본과의 호환성을 위해 유지합니다. 기존 InputLeap 사용자 설정은 InputLeafPlus 최초 실행 시 자동으로 가져옵니다.

이 프로젝트는 원본 Input Leap과 동일하게 GNU General Public License v2에 따라 배포됩니다. 자세한 내용은 [LICENSE](LICENSE)를 확인하세요.
