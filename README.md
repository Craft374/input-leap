# InputLeafPlus

InputLeafPlus는 한 대의 키보드와 마우스로 여러 컴퓨터를 제어하는 오픈 소스 소프트웨어입니다. [Input Leap](https://github.com/input-leap/input-leap)을 기반으로 하며, 특히 Windows와 macOS를 함께 사용할 때 필요한 입력 변환, 클립보드 안정성, 빌드 편의성을 보강했습니다.

이 저장소의 개선 사항은 원본 프로젝트의 `master` 기준 커밋 `34a34fb2`와 비교해 정리했습니다.

## 원본 대비 개선 사항

### Windows와 macOS 사이의 키보드 입력

- 화면별 키 매핑 설정 창을 추가했습니다. 서버의 키를 각 클라이언트에서 어떤 키로 보낼지 GUI에서 지정할 수 있습니다.
- Shift, Ctrl, Alt, Meta, Super 키를 화면별로 다시 배치할 수 있습니다.
- Windows의 `Ctrl+C/V` 같은 단축키를 macOS의 `Command+C/V`로 보내는 프리셋과 기존 프리셋 자동 마이그레이션을 제공합니다.
- Caps Lock을 F18로 바꾸는 프리셋을 제공합니다.
- Caps Lock을 다른 키로 매핑해 원격 화면에서 사용할 때 Windows 로컬 Caps Lock 상태가 함께 바뀌지 않도록 처리했습니다.
- 키 누름, 키 뗌, 키 반복, 키보드 브로드캐스트에 동일한 화면별 매핑을 적용합니다.
- 매핑된 보조 키의 원래 modifier 상태가 함께 전달되어 조합키로 오인되는 문제를 방지했습니다.
- Windows 한/영 키의 가상 키 코드 매핑을 바로잡았습니다.
- macOS에서 F17~F20 확장 기능 키를 인식하도록 추가했습니다.

### 화면별 스크롤 설정

- 클라이언트 화면마다 스크롤 방향을 반대로 설정할 수 있습니다.
- 이전 개발 버전의 `reverseMouse` 설정은 `reverseScroll`로 자동 호환됩니다.

### 클립보드 공유 안정성

- macOS의 `public.utf8-plain-text` 형식을 지원해 한글을 포함한 UTF-8 텍스트 전달을 보강했습니다.
- Windows와 macOS 사이에서 텍스트 줄바꿈을 각 운영체제 형식에 맞게 변환합니다.
- 화면을 전환하는 순간 서버 클립보드를 즉시 읽어 최신 내용이 빠지는 문제를 줄였습니다.
- 클립보드 소유권 알림 순서가 엇갈릴 때 발생하던 잘못된 업데이트와 충돌 가능성을 방어했습니다.
- macOS 클립보드 쓰기 결과를 실제 성공 여부대로 반환하고 Core Foundation 객체를 정리합니다.
- 클립보드 사용 여부와 최대 크기 옵션을 함께 안정적으로 처리하며, 값이 빠진 잘못된 옵션도 안전하게 무시합니다.

### Windows 서비스 완전 종료

- Windows 서비스는 자동 시작이 아닌 수동 시작으로 설치됩니다.
- InputLeafPlus를 실행할 때 서비스 모드라면 서비스도 함께 시작됩니다.
- 트레이 아이콘의 `종료`를 누르면 클라이언트 또는 서버 프로세스를 정상 종료한 뒤 백그라운드 서비스까지 중지합니다.
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

```powershell
.\build_windows.bat
```

빌드 결과는 `build\input-leap-install`에 생성됩니다. Inno Setup 6이 설치되어 있으면 `build\installer-inno\bin`에 InputLeafPlus 설치 파일도 생성됩니다.

### macOS Apple Silicon

```bash
chmod +x build_macos.command
./build_macos.command
```

완성된 앱은 `build/bundle/InputLeafPlus.app`에 생성됩니다.

## 호환성과 라이선스

설정 파일 형식, 네트워크 프로토콜, `input-leap`, `input-leaps`, `input-leapc`, `input-leapd` 실행 파일명은 원본과의 호환성을 위해 유지합니다. 기존 InputLeap 사용자 설정은 InputLeafPlus 최초 실행 시 자동으로 가져옵니다.

이 프로젝트는 원본 Input Leap과 동일하게 GNU General Public License v2에 따라 배포됩니다. 자세한 내용은 [LICENSE](LICENSE)를 확인하세요.
