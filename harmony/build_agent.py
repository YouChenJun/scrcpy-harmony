"""Build and sign the media companion with the SDK's bundled development key."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
BUNDLE = "org.scrcpy.harmony.agent"


def build_agent(sdk: Path, target: str, hdc: str, app: Path = None, bundle: str = BUNDLE, name: str = "agent") -> Path:
    app = app or ROOT / "harmony/agent"
    output = ROOT / "build-harmony"
    output.mkdir(exist_ok=True)
    signed = output / ("scrcpy-harmony-" + name + ".hap")
    stamp = output / (name + "-build.json")
    sources = [p for p in app.rglob("*") if p.is_file()
               and not any(part in {"build", ".hvigor", ".cxx", "oh_modules", "node_modules"} for part in p.relative_to(app).parts)
               and p.name != "local.properties"]
    sources += list((ROOT / "harmony").glob("*.h")) + [ROOT / "harmony/server.cpp", Path(__file__)]
    digest = hashlib.sha256()
    for file in sorted(sources):
        digest.update(str(file.relative_to(ROOT)).encode()); digest.update(file.read_bytes())
    digest.update(str(sdk).encode()); digest.update(target.encode())
    fingerprint = digest.hexdigest()
    cached = json.loads(stamp.read_text()) if stamp.exists() else {}
    if (not signed.exists() or cached.get("source") != fingerprint
            or cached.get("expires", 0) < time.time() + 86400):
        clt = sdk.parents[3]
        hvigor = clt / "bin/hvigorw"
        if not hvigor.is_file():
            raise RuntimeError("The media backend needs HarmonyOS command-line-tools (hvigorw). Use --backend=root for the native-only backend.")
        java_home = Path(os.environ.get("JAVA_HOME", "/opt/homebrew/opt/openjdk"))
        java = str(java_home / "bin/java") if (java_home / "bin/java").is_file() else shutil.which("java")
        if not java:
            raise RuntimeError("Java is required to package and sign the media companion")
        lib = sdk.parent / "toolchains/lib"
        env = os.environ.copy()
        env["DEVECO_SDK_HOME"] = str(sdk.parents[2])
        env["JAVA_HOME"] = str(Path(java).resolve().parent.parent)
        env["PATH"] = str(Path(java).parent) + os.pathsep + env.get("PATH", "")
        (app / "local.properties").write_text("sdk.dir=" + str(sdk.parents[2]) + "\n")
        subprocess.run([str(clt / "bin/ohpm"), "install", "--all"], cwd=app, env=env, check=True)
        subprocess.run([str(hvigor), "--mode", "module", "-p", "product=default", "-p", "module=entry@default",
                        "-p", "buildMode=debug", "assembleHap", "--no-daemon"], cwd=app, env=env, check=True)
        signing = output / (name + "-signing"); signing.mkdir(exist_ok=True)
        profile = json.loads((lib / "UnsgnedDebugProfileTemplate.json").read_text())
        leaf = profile["bundle-info"]["development-certificate"]
        certificates = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                                  (lib / "OpenHarmonyProfileDebug.pem").read_text(), re.S)
        # SDK PEM order is root, application CA, profile leaf. The application
        # leaf is in the profile template and matches the SDK development key.
        chain = signing / "application-chain.pem"
        chain.write_text(leaf + "\n" + certificates[1] + "\n" + certificates[0] + "\n")
        device = subprocess.check_output([hdc, "-t", target, "shell", "bm get -u"], text=True)
        ids = re.findall(r"\b[A-Fa-f0-9]{64}\b", device)
        if not ids:
            raise RuntimeError("Could not obtain the device identifier for the companion's development profile")
        profile["uuid"] = str(uuid.uuid4())
        profile["validity"] = {"not-before": int(time.time()) - 3600, "not-after": int(time.time()) + 30 * 86400}
        profile["bundle-info"]["bundle-name"] = bundle
        profile["debug-info"]["device-ids"] = [ids[0]]
        profile_json = signing / "profile.json"; profile_json.write_text(json.dumps(profile))
        signer = [java, "-jar", str(lib / "hap-sign-tool.jar")]
        # Public key material/password shipped in the SDK; never a user's key.
        common = ["-mode", "localSign", "-signAlg", "SHA256withECDSA", "-keystoreFile", str(lib / "OpenHarmony.p12"),
                  "-keystorePwd", "123456", "-keyPwd", "123456"]
        subprocess.run([*signer, "sign-profile", *common, "-keyAlias", "openharmony application profile debug",
                        "-profileCertFile", str(lib / "OpenHarmonyProfileDebug.pem"), "-inFile", str(profile_json),
                        "-outFile", str(signing / "profile.p7b")], check=True)
        subprocess.run([*signer, "sign-app", *common, "-keyAlias", "openharmony application release",
                        "-appCertFile", str(chain), "-profileFile", str(signing / "profile.p7b"),
                        "-inFile", str(app / "entry/build/default/outputs/default/entry-default-unsigned.hap"),
                        "-outFile", str(signed), "-compatibleVersion", "12"], check=True)
        stamp.write_text(json.dumps({"source": fingerprint, "expires": profile["validity"]["not-after"]}))
    install_stamp = output / (name + "-install-" + hashlib.sha256(target.encode()).hexdigest()[:12] + ".json")
    package_hash = hashlib.sha256(signed.read_bytes()).hexdigest()
    installed = json.loads(install_stamp.read_text()) if install_stamp.exists() else {}
    present = subprocess.check_output([hdc, "-t", target, "shell", "bm dump -n " + bundle], text=True)
    if installed.get("package") != package_hash or bundle not in present:
        result = subprocess.run([hdc, "-t", target, "install", "-r", str(signed)], capture_output=True, text=True, check=True)
        if "install bundle successfully" not in result.stdout:
            raise RuntimeError("Companion installation failed: " + result.stdout.strip())
        install_stamp.write_text(json.dumps({"package": package_hash}))
        print("[harmony] media companion installed", flush=True)
    return signed
