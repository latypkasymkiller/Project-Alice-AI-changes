# Project Alice no Android

Port para Android do [Project Alice](https://github.com/schombert/Project-Alice), um motor de código aberto para jogar Victoria 2. O jogo roda nativamente: o mesmo código C++ do desktop é compilado para ARM64 e desenha com OpenGL ES 3.2.

> **Situação:** versão de testes. Já roda num aparelho real (Moto One Hyper, Android 11, Adreno 612), mas espere problemas. Veja [Limitações conhecidas](#limitações-conhecidas).
>
> **[⬇️ Baixar a última versão (APK)](https://github.com/Vitorio-Kuroski/Project-Alice-mobile/releases/latest/download/ProjectAlice-Android-arm64.apk)**

## Para jogar

### Requisitos

- Android 11 ou mais novo (API 30+)
- Celular ou tablet ARM 64 bits (arm64-v8a), que é a grande maioria dos aparelhos atuais
- GPU com OpenGL ES 3.2
- Os arquivos do **Victoria 2 com as expansões**, copiados para o aparelho. O Project Alice não inclui o conteúdo do jogo original.
- Espaço livre: cerca de 150 MB para o app e os arquivos dele, além da pasta do Victoria 2

### Instalação

1. Baixe o APK pelo [link da última versão](https://github.com/Vitorio-Kuroski/Project-Alice-mobile/releases/latest/download/ProjectAlice-Android-arm64.apk); as anteriores ficam em [Releases](https://github.com/Vitorio-Kuroski/Project-Alice-mobile/releases). Para testar algo ainda não publicado, use a aba **Actions**. Abra a execução mais recente do workflow "Android port - build do APK (arm64-v8a)" e baixe o artefato `ProjectAlice-arm64-v8a-android-arm64-release-apk` (o build otimizado; o de `android-arm64` é o de debug, bem mais lento).
2. Copie a pasta do Victoria 2 para o aparelho, por exemplo para `Jogos/Victoria 2` no armazenamento interno. A pasta certa é a que contém `common`, `map` e `gfx`. Evite a pasta `Download` e a raiz do armazenamento: o seletor de pastas do Android 11+ não permite escolher essas.
3. Instale o APK (é preciso permitir a instalação de fontes desconhecidas) ou use `adb install app-debug.apk`.

### Primeira execução

1. O app prepara os arquivos do próprio Alice (shaders, fontes, sons, textos). Isso leva alguns segundos e só se repete depois de uma atualização.
2. Aparece o aviso **"Pasta do Victoria 2"**. Toque em **Escolher pasta**:
   - o Android pede a permissão **"Acesso a todos os arquivos"**; ative-a e volte ao app (o jogo lê os arquivos direto da pasta);
   - no seletor, abra a pasta do Victoria 2 e toque em **Usar esta pasta**.
3. Na primeira vez o app **monta o cenário** a partir dos arquivos do jogo, o que pode levar vários minutos. Enquanto isso o fundo da tela pulsa. Nas próximas vezes o cenário já está pronto e o jogo abre direto.

A pasta escolhida fica salva. Para trocá-la, apague o arquivo `game_folder.txt` (ver [Onde ficam os arquivos](#onde-ficam-os-arquivos)) ou limpe os dados do app.

Na primeira vez o app ajusta duas opções para o aparelho. As duas podem ser mudadas depois nas opções do jogo:
- **Escala da interface:** escolhida pela tela, para os botões ficarem num tamanho bom para o dedo sem que as janelas do jogo deixem de caber.
- **Antialiasing (MSAA): desligado.** Em GPU de celular ele custa caro e, em telas de ~400 dpi, quase não se vê o serrilhado.

### Desempenho

Para aliviar a GPU, o jogo **desenha em 75% da resolução da tela** e o hardware de vídeo do aparelho amplia a imagem, sem custo. Numa tela de 2340×1080, ele desenha em 1755×810, cerca de 44% menos pixels no shader do mapa, que é o mais pesado. O texto fica um pouco menos nítido. O valor fica em `render_scale` (`src/window/window.hpp`); ainda não há opção no jogo para mudá-lo.

### Carregamentos

Ao entrar na partida o jogo monta toda a interface de uma vez, e no celular isso leva alguns segundos. O mesmo vale para carregar um save. Nesses momentos aparece a janela **"Carregando..."** do Android. Ela recebe os toques enquanto o jogo está ocupado, o que evita o aviso de "o app não está respondendo".

### Sair no meio da partida

Ao trocar de app ou bloquear a tela durante uma partida, o jogo **pausa** e **salva** em `android_autosave.bin`, porque o Android pode fechar apps em segundo plano sem avisar. Para continuar depois, carregue esse save. O arquivo é sempre o mesmo (sobrescrito) e é gravado no máximo uma vez a cada 30 s. Só vale para o modo single-player.

### Controles

| Gesto | Equivale a |
|---|---|
| Toque rápido | Clique esquerdo (selecionar, botões) |
| Segurar parado por ~0,5 s | Clique direito (ordem de movimento, menu da província) |
| Arrastar sobre a interface | Arrastar com o botão esquerdo (barras de rolagem, janelas) |
| Arrastar sobre o mapa | Mover o mapa |
| Pinça com dois dedos | Zoom (mover os dois dedos também arrasta o mapa) |
| Toque com três dedos | Mostra ou esconde o contador de desempenho: FPS, tempo médio e pior quadro, velocidade do jogo e dias simulados por segundo |
| Botão ou gesto de voltar | ESC (menu do jogo) |

Mouse (com os três botões e a roda) e teclado físico, por USB ou Bluetooth, funcionam como no desktop.

### Onde ficam os arquivos

| O quê | Onde |
|---|---|
| Configurações, cenários, saves, `scenario_errors.log` | `Android/data/org.projectalice.mobile/files/.local/share/Alice/` (acessível pelo cabo USB) |
| Pasta do Victoria 2 escolhida | `Android/data/org.projectalice.mobile/files/game_folder.txt` |
| Arquivos do Alice extraídos do APK | armazenamento interno do app (`/data/data/org.projectalice.mobile/files/alice`) |

O app nunca escreve na pasta do Victoria 2.

### Problemas? Mande o log

```bash
adb logcat -s Alice
```

O log mostra cada etapa: extração dos arquivos, pasta escolhida, montagem e carregamento do cenário, versão do OpenGL ES e GPU, e erros de shader ou textura.

## Para desenvolver

### Compilar pelo GitHub Actions

O workflow `.github/workflows/android-port-fase1.yml` roda manualmente. Na aba **Actions**, escolha "Android port - build do APK (arm64-v8a)", clique em **Run workflow**, selecione a branch, o preset e, se quiser publicar, a **Tag do release** (ex.: `v0.3`); com ela preenchida o workflow cria um release no GitHub com o APK e as notas de `android/release-notes.md`. Presets: `android-arm64-release` (padrão, otimizado, para jogar) ou `android-arm64` (debug, com asserções, para investigar erros). Ele:

1. valida os shaders como GLSL ES 3.20 e GLSL 4.60 (`android/validate-shaders.sh`);
2. compila no Linux as ferramentas que geram código durante o build;
3. compila a `libAlice.so` para arm64-v8a com o NDK r29, no preset escolhido;
4. gera o APK com o Gradle e o publica como artefato, junto com o log do build.

### Assinatura com chave fixa

O Android só instala um APK por cima de outro se os dois tiverem a mesma assinatura. Sem chave própria, cada execução do Actions assina com uma chave de debug nova, e o usuário precisa desinstalar o app (perdendo saves e configurações) a cada versão. Com a chave fixa guardada nos segredos do repositório, toda versão nova atualiza a anterior.

Um release (campo **Tag do release** preenchido) **exige** a chave: sem ela o workflow para logo no início, a não ser que a opção **release_sem_chave** esteja marcada. Um build sem tag continua funcionando sem a chave, com um aviso.

**1. Criar a chave** (uma vez só), num PC com JDK (o `keytool` vem com o Java e com o Android Studio) ou no próprio celular pelo [Termux](https://termux.dev) (`pkg install openjdk-17`):

```bash
keytool -genkeypair -keystore alice-release.p12 -storetype PKCS12 -alias alice \
  -keyalg RSA -keysize 2048 -validity 36500 -dname "CN=Project Alice mobile"
```

Ele pede uma senha, que vale para a chave e para o arquivo. Depois converta o arquivo para texto:

```bash
base64 -w0 alice-release.p12 > alice-release.b64     # Linux, Termux
```
```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("alice-release.p12")) | Set-Content alice-release.b64   # Windows
```

**2. Guardar no GitHub:** em **Settings → Secrets and variables → Actions → New repository secret**, crie:

| Nome | Valor |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | o conteúdo de `alice-release.b64` |
| `ANDROID_KEYSTORE_PASSWORD` | a senha escolhida |

**3. Fazer backup** do `alice-release.p12` e da senha num lugar seguro, fora do repositório. O GitHub não deixa ler um segredo de volta. Se a chave se perder, a versão seguinte volta a exigir desinstalar o app. Nunca coloque a chave no repositório: com ela qualquer um assina um APK que se passa pelo app.

O passo "Conferir a assinatura do APK" do workflow mostra o SHA-256 do certificado. Ele deve ser o mesmo em todos os releases.

Localmente, o Gradle usa a chave fixa quando encontra `ALICE_KEYSTORE` (caminho do `.p12`) e `ALICE_KEYSTORE_PASSWORD` no ambiente; sem elas, usa a chave de debug da máquina.

### Compilar localmente

Precisa de: CMake 3.28+, Ninja, clang, **Android NDK r29**, Android SDK (plataforma 35, build-tools 35), JDK 17+ e, no Linux, os pacotes de desenvolvimento de X11/OpenGL/ICU para as ferramentas de host.

```bash
# 1. ferramentas de geração de código (rodam no PC durante o build Android)
cmake --preset x64-debug-linux-clang
cmake --build out/build/x64-debug-linux-clang --target \
  ParserGenerator DCONGENERATOR DCONINTERFACEGEN DataContainer-Lua DataContainer_OOS_Reporter
mkdir -p native-tools
find out/build/x64-debug-linux-clang -type f \( -name ParserGenerator -o -name DCONGENERATOR \
  -o -name DCONINTERFACEGEN -o -name DataContainer-Lua -o -name DataContainer_OOS_Reporter \) \
  -exec cp {} native-tools/ \;

# 2. a biblioteca nativa
export ANDROID_NDK=/caminho/para/android-ndk-r29
cmake --preset android-arm64-release -DALICE_NATIVE_TOOLS_DIR=$PWD/native-tools   # ou android-arm64 (debug)
cmake --build out/build/android-arm64-release --target Alice

# 3. o APK
export ANDROID_HOME=/caminho/para/android-sdk
android/collect-native-libs.sh out/build/android-arm64-release   # copia as .so para o Gradle (sem símbolos)
cd android && ./gradlew assembleDebug   # -> app/build/outputs/apk/debug/app-debug.apk
```

Para validar os shaders depois de mexer neles (precisa do `glslang-tools`):

```bash
android/validate-shaders.sh
```

### Como o port funciona

O código nativo é compilado pelo CMake, não pelo Gradle. O Gradle só empacota a `.so`, a Activity Java e os arquivos do Alice, e assina o APK.

| Parte | Arquivo(s) | O que faz |
|---|---|---|
| Entrada e carregamento | `src/entry_point_android.cpp` | `android_main`; extrai os arquivos do Alice, acha a pasta do Victoria 2, monta ou carrega o cenário numa thread separada e inicia o jogo |
| Janela, EGL e entrada | `src/window/window_android.cpp` | Ciclo de vida da NativeActivity, contexto EGL (GLES 3.2), loop de render, toque, mouse e teclado |
| Contexto OpenGL | `src/graphics/opengl_wrapper_android.cpp` | `eglMakeCurrent`, vsync e saída de debug |
| OpenGL de desktop sobre GLES | `src/graphics/android_gl/glew.h` | Substitui o GLEW: implementa com GLES as funções de desktop que o jogo usa e descomprime texturas S3TC/DXT quando a GPU não suporta |
| Shaders | `assets/shaders/glsl/*`, `ogl::set_shader_prefix` | Os mesmos arquivos valem como GLSL 4.60 e GLSL ES 3.20; no Android o cabeçalho `#version` é trocado na hora |
| SSE/AVX no ARM | `src/x86_compat/`, `dependencies/simde` | Headers de intrínsecas x86 redirecionados para o [SIMDe](https://github.com/simd-everywhere/simde), que usa NEON |
| Activity Java | `android/app/src/main/java/.../AliceActivity.java` | Permissão de arquivos, seletor de pastas e mensagens; o resto é C++ |
| Empacotamento | `android/app/build.gradle.kts`, `android/collect-native-libs.sh` | `.so` em `jniLibs`; `assets/` do repositório dentro do APK com uma lista (com CRC32) para a extração |

Outras decisões que valem saber:

- **Otimizações para GPU de celular:** renderização em 75% da resolução (`ANativeWindow_setBuffersGeometry`; toque e mouse são convertidos para a escala do desenho), MSAA desligado por padrão, `glInvalidateFramebuffer` do depth/stencil antes de cada troca de quadro (evita gravá-los na memória numa GPU que desenha em blocos) e `-mtune=cortex-a76` (só ajusta a escolha de instruções; continua rodando em qualquer arm64).
- **LuaJIT** é linkado de forma estática no Android. A `.so` dele tem nome com versão (`libluajit-5.1.so.2`), que o Android não carrega de dentro do APK.
- **ICU:** o jogo só usa a quebra de texto do ICU (caracteres, palavras e linhas). No Android 12+ ela vem do sistema (`libicu.so`, carregada com `dlopen`). No Android 11, que não tem ICU público para apps, entra uma implementação própria em `src/text/icu_android_compat.hpp`. Comparada com o ICU nos textos de localização do Alice, ela dá a mesma quebra de linha em ~99% das frases em inglês, alemão e russo; chinês e japonês ficam aproximados.
- **Sistema de arquivos:** é o mesmo do Linux (`simple_fs_nix.cpp`). `$HOME` aponta para a pasta externa do app, e as raízes do jogo são `[pasta do Victoria 2, pasta interna com assets/]`.
- **Fechar o app** encerra o processo: as threads do jogo e o estado estático não podem ser reaproveitados por uma nova Activity.

### Limitações conhecidas

- **Testado em um único aparelho** até agora (Moto One Hyper, Adreno 612). Outras GPUs (Mali, PowerVR) ainda não foram testadas.
- **Mods:** só o jogo base; falta uma tela de seleção de mods.
- **Digitar texto** (nome de save, chat) não funciona: a API nativa não entrega os caracteres do teclado virtual. Isso precisa de uma ponte JNI.
- **Seleção em caixa** de várias unidades com o dedo ainda não existe; dá para selecionar uma por vez.
- **Retomar depois de o Android fechar o app:** o save de segundo plano é feito, mas é preciso carregá-lo manualmente pelo menu.
- **Valores de toque** (tempo do segurar, tolerância de movimento, velocidade do zoom da pinça) são palpites a ajustar com testes.
- **Só arm64-v8a.**
- **Memória:** texturas S3TC descomprimidas ocupam mais memória de vídeo em GPUs sem suporte a S3TC (Mali, PowerVR).
- **DataContainer** é baixado da branch `master` sem versão fixa, e uma mudança lá pode quebrar o build.
