$ErrorActionPreference = 'Stop'
# JDK 17 + Android SDK (build-tools 34.0.0, platform android-34). Set JT2_JDK/JT2_SDK to override.
# debug.keystore is NOT committed; the script generates one in this dir on first run.
$jdk = $env:JT2_JDK
if (-not $jdk) { $jdk = "d:\AItools\projects\jt2.0\_work\tools\jdk17\jdk-17.0.21+9" }
$sdk = $env:JT2_SDK
if (-not $sdk) { $sdk = "F:\Android\sdk" }
$root      = $PSScriptRoot
$bt        = "$sdk\build-tools\34.0.0"
$androidJar= "$sdk\platforms\android-34\android.jar"
$srcH5     = Join-Path $root "..\index.html"
$outApk    = Join-Path $root "jianti-2.0.0-debug.apk"
$delivApk  = Join-Path $root "..\jianti-2.0.0-debug.apk"

$env:JAVA_HOME = $jdk
$env:PATH = "$jdk\bin;$env:PATH"

# 清理
foreach ($d in @("$root\build", "$root\gen", "$root\assets")) {
  if (Test-Path $d) { Remove-Item $d -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path "$root\build", "$root\gen", "$root\assets" | Out-Null

# H5 打进 assets
Copy-Item $srcH5 "$root\assets\index.html"

# 图标资源（res/mipmap-*）先编译成扁平包，再参与链接
& "$bt\aapt2.exe" compile --dir "$root\res" -o "$root\build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 compile failed" }

# 资源链接 + 生成 R.java
& "$bt\aapt2.exe" link -o "$root\build\base.apk" --manifest "$root\AndroidManifest.xml" -I $androidJar -A "$root\assets" -R "$root\build\res.zip" --java "$root\gen" --auto-add-overlay
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

# 编译 Java
$javaFiles = @()
$javaFiles += (Get-ChildItem "$root\src" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
$javaFiles += (Get-ChildItem "$root\gen" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
New-Item -ItemType Directory -Force -Path "$root\build\classes" | Out-Null
# JDK17 用 -source/-target 8 会往 stderr 打 bootstrap warning；PS5 在 Stop 模式下会把
# native stderr 误当终止错误。临时切 Continue，真失败仍由 $LASTEXITCODE 拦住。
$_prevEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& "$jdk\bin\javac.exe" -source 8 -target 8 -nowarn -encoding UTF-8 -classpath $androidJar -d "$root\build\classes" $javaFiles 2>$null
$ErrorActionPreference = $_prevEAP
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

# dex
& "$jdk\bin\jar.exe" cf "$root\build\classes.jar" -C "$root\build\classes" .
if ($LASTEXITCODE -ne 0) { throw "jar cf failed" }
New-Item -ItemType Directory -Force -Path "$root\build\dex" | Out-Null
& "$bt\d8.bat" --release --lib $androidJar --min-api 24 --output "$root\build\dex" "$root\build\classes.jar"
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }

# classes.dex 入包
Push-Location "$root\build\dex"
& "$jdk\bin\jar.exe" uf "$root\build\base.apk" "classes.dex"
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "jar uf failed" }

# 对齐
& "$bt\zipalign.exe" -f 4 "$root\build\base.apk" "$root\build\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

# 调试签名
if (-not (Test-Path "$root\debug.keystore")) {
  & "$jdk\bin\keytool.exe" -genkeypair -keystore "$root\debug.keystore" -alias androiddebugkey `
    -storepass android -keypass android -keyalg RSA -keysize 2048 -validity 10000 `
    -dname "CN=Android Debug,O=Android,C=US"
  if ($LASTEXITCODE -ne 0) { throw "keytool failed" }
}
if (Test-Path $outApk) { Remove-Item $outApk -Force }
& "$bt\apksigner.bat" sign --ks "$root\debug.keystore" --ks-pass pass:android --key-pass pass:android `
  --min-sdk-version 24 --out $outApk "$root\build\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

& "$bt\apksigner.bat" verify --min-sdk-version 24 $outApk
Copy-Item $outApk $delivApk -Force
$f = Get-Item $outApk
"SIZE={0} SHA256={1}" -f $f.Length, (Get-FileHash $outApk -Algorithm SHA256).Hash
