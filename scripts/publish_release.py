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
    file_name = "FoloToy-AI-Passport-full.bin"
    with open(bin_path, 'rb') as f:
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
        print(f"[Release] Failed to upload asset: {e.code} - {err}")

if __name__ == '__main__':
    main()
