#!/usr/bin/env bash
#
# Provision a throwaway Windows 10 22H2 VM on Azure, run the VDD repro harness
# on it for one or more driver versions, collect the JSON results, and tear the
# whole thing down.
#
# Why this exists: GitHub-hosted runners are Server 2022 (build 20348), a
# different servicing branch from Win10 22H2 (build 19045). A clean run there
# proves the driver bits are fine but says nothing about 22H2 specifically.
#
# Everything runs through `az vm run-command`, which executes as LocalSystem
# over the Azure agent -- so the VM needs no public IP and no inbound rules.
#
# Usage:
#   ./Invoke-AzureWin10Repro.sh v0.14.4 v0.15.0 v0.15.4
#
set -euo pipefail

VERSIONS=("$@")
if [ ${#VERSIONS[@]} -eq 0 ]; then
    VERSIONS=(v0.14.4 v0.15.0 v0.15.1 v0.15.4)
fi

: "${AZ_LOCATION:=eastus}"
: "${AZ_VM_SIZE:=Standard_D2s_v5}"
# Windows 10 Enterprise 22H2. Client images require an eligible subscription
# (Visual Studio / dev-test, or AVD licensing); a plain pay-as-you-go sub will
# be refused at create time with a licensing error.
: "${AZ_IMAGE:=MicrosoftWindowsDesktop:windows-10:win10-22h2-ent:latest}"
: "${AZ_BRANCH:=ci/vdd-repro-harness}"
: "${AZ_REPO:=qiin2333/zako-vdd}"

STAMP="$(date +%m%d%H%M)"
RG="rg-vddrepro-${STAMP}"
VM="vdd${STAMP}"
ADMIN_USER="vddadmin"
ADMIN_PASS="Vdd!$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16)"
OUTDIR="./azure-results-${STAMP}"

RAW_BASE="https://raw.githubusercontent.com/${AZ_REPO}/${AZ_BRANCH}/tools/vdd-repro"

cleanup() {
    echo "==> Deleting resource group ${RG} (background)"
    az group delete --name "$RG" --yes --no-wait 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$OUTDIR"

echo "==> Subscription: $(az account show --query 'name' -o tsv)"
echo "==> Creating ${RG} in ${AZ_LOCATION}"
az group create --name "$RG" --location "$AZ_LOCATION" --output none

echo "==> Creating VM ${VM} (${AZ_VM_SIZE}, ${AZ_IMAGE})"
# No public IP: run-command reaches the VM through the Azure agent, so there is
# nothing to expose and no NSG rule to get wrong.
az vm create \
    --resource-group "$RG" \
    --name "$VM" \
    --image "$AZ_IMAGE" \
    --size "$AZ_VM_SIZE" \
    --admin-username "$ADMIN_USER" \
    --admin-password "$ADMIN_PASS" \
    --public-ip-address "" \
    --nsg "" \
    --license-type Windows_Client \
    --output none

echo "==> VM ready; running harness for: ${VERSIONS[*]}"

# The bootstrap runs once and stays on the VM; each version then reuses it.
BOOTSTRAP=$(cat <<'PS'
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Path C:\vddrepro -Force | Out-Null
Invoke-WebRequest -UseBasicParsing -Uri "__RAW__/Invoke-VddRepro.ps1" -OutFile C:\vddrepro\Invoke-VddRepro.ps1
$nef = 'https://github.com/nefarius/nefcon/releases/download/v1.17.40/nefcon_v1.17.40.zip'
Invoke-WebRequest -UseBasicParsing -Uri $nef -OutFile C:\vddrepro\nefcon.zip
Expand-Archive C:\vddrepro\nefcon.zip -DestinationPath C:\vddrepro\nefcon -Force
$os = Get-CimInstance Win32_OperatingSystem
"$($os.Caption) build $($os.BuildNumber)"
PS
)
BOOTSTRAP="${BOOTSTRAP//__RAW__/$RAW_BASE}"

echo "--- bootstrap"
az vm run-command invoke --resource-group "$RG" --name "$VM" \
    --command-id RunPowerShellScript --scripts "$BOOTSTRAP" \
    --query 'value[0].message' -o tsv

for ver in "${VERSIONS[@]}"; do
    echo "--- ${ver}"
    RUN=$(cat <<'PS'
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ver = '__VER__'
$dir = "C:\vddrepro\$ver"
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$url = "https://github.com/__REPO__/releases/download/$ver/zakovdd.zip"
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$dir\zakovdd.zip"
Expand-Archive "$dir\zakovdd.zip" -DestinationPath "$dir\pkg" -Force
$inf = Get-ChildItem -Path "$dir\pkg" -Filter ZakoVDD.inf -Recurse | Select-Object -First 1
& C:\vddrepro\Invoke-VddRepro.ps1 -DriverDirectory $inf.DirectoryName `
    -NefconPath (Get-ChildItem C:\vddrepro\nefcon -Filter nefconw.exe -Recurse |
        Where-Object { $_.FullName -match 'x64' } | Select-Object -First 1).FullName `
    -OutputJson "$dir\result.json" -Label $ver *>&1 | Out-Null
Get-Content "$dir\result.json" -Raw
PS
)
    RUN="${RUN//__VER__/$ver}"
    RUN="${RUN//__REPO__/$AZ_REPO}"

    az vm run-command invoke --resource-group "$RG" --name "$VM" \
        --command-id RunPowerShellScript --scripts "$RUN" \
        --query 'value[0].message' -o tsv > "$OUTDIR/${ver}.raw" || true

    # run-command wraps stdout in a stdout/stderr banner; keep only the JSON.
    python3 - "$OUTDIR/${ver}.raw" "$OUTDIR/${ver}.json" <<'PY' || echo "    (no JSON for $ver)"
import json, re, sys
raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r"\{.*\}", raw, re.S)
if not m:
    sys.exit(1)
data = json.loads(m.group(0))
json.dump(data, open(sys.argv[2], "w"), indent=2)
print("    verdict={} createmonitor={} enumerated={} pnpAdded={}".format(
    data.get("verdict"), data.get("createMonitorWin32"),
    data.get("monitorEnumerated"), data.get("pnpMonitorsAdded")))
PY
done

echo
echo "==> Results in ${OUTDIR}"
