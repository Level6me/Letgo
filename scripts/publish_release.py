import os
import sys
import json
import urllib.request
import urllib.error

def main():
    token = os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN')
    repo = os.environ.get('GITHUB_REPOSITORY')
    run_id = os.environ.get('GITHUB_RUN_ID', 'manual')
    bin_path = sys.argv[1] if len(sys.argv) > 1 else 'build/merged-binary.bin'

    if not token or not repo:
        print("[Release] Missing GH_TOKEN or GITHUB_REPOSITORY, skipping.")
        return

    if not os.path.exists(bin_path):
        print(f"[Release] Binary not found at {bin_path}, skipping.")
        return

    tag = f"fw-{run_id}"
    print(f"[Release] Creating release for {repo} tag {tag}...")

    # 1. 创建 Release
    url = f"https://api.github.com/repos/{repo}/releases"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json",
        "User-Agent": "Letgo-Release-Bot"
    }
    data = json.dumps({
        "tag_name": tag,
        "name": f"Release {tag}",
        "body": f"FoloToy AI Passport xiaozhi-based firmware binary for run {run_id}",
        "draft": False,
        "prerelease": False
    }).encode('utf-8')

    try:
        req = urllib.request.Request(url, data=data, headers=headers, method='POST')
        with urllib.request.urlopen(req) as resp:
            rel = json.loads(resp.read().decode('utf-8'))
            upload_url = rel['upload_url'].split('{')[0]
    except urllib.error.HTTPError as e:
        err = e.read().decode('utf-8')
        print(f"[Release] Failed to create release: {e.code} - {err}")
        return

    # 2. 上传二进制资产
    assets_to_upload = [("FoloToy-AI-Passport-full.bin", bin_path)]

    for ota_name in ["build/ai-passport.bin", "build/xiaozhi.bin", "build/xiao_zhi.bin"]:
        if os.path.exists(ota_name):
            assets_to_upload.append(("FoloToy-AI-Passport-ota.bin", ota_name))
            break

    if len(assets_to_upload) == 1 and os.path.exists(bin_path):
        try:
            with open(bin_path, 'rb') as f:
                f.seek(0x1A0000)
                ota_slice = f.read()
            if len(ota_slice) > 0 and ota_slice[0] == 0xe9:
                slice_path = "build/extracted_ota.bin"
                os.makedirs("build", exist_ok=True)
                with open(slice_path, 'wb') as f_out:
                    f_out.write(ota_slice)
                assets_to_upload.append(("FoloToy-AI-Passport-ota.bin", slice_path))
        except Exception as e:
            print(f"[Release] Extract OTA slice fallback warning: {e}")

    for file_name, file_path in assets_to_upload:
        if not os.path.exists(file_path):
            continue
        with open(file_path, 'rb') as f:
            content = f.read()

        print(f"[Release] Uploading {file_name} ({len(content)} bytes)...")
        upload_headers = {
            "Authorization": f"token {token}",
            "Content-Type": "application/octet-stream",
            "User-Agent": "Letgo-Release-Bot"
        }
        upload_url_full = f"{upload_url}?name={file_name}"
        try:
            req_up = urllib.request.Request(upload_url_full, data=content, headers=upload_headers, method='POST')
            with urllib.request.urlopen(req_up) as resp:
                print(f"[Release] Successfully published {file_name} to release {tag}!")
        except urllib.error.HTTPError as e:
            err = e.read().decode('utf-8')
            print(f"[Release] Failed to upload asset {file_name}: {e.code} - {err}")

if __name__ == '__main__':
    main()
