# Скриншот ВМ 402 на Proxmox (подэтап 49.1): screendump через qm monitor,
# чтение файла через разрешённое правило sudo, копия в build/pve402/<имя>.png.
# Шлюз иногда не отвечает — каждый шаг с повторами.
#   powershell -File scripts/pve402_shot.ps1 <имя>
param([string]$name = "shot")
$dir = "D:\OS2\build\pve402"
New-Item -ItemType Directory -Force $dir | Out-Null
$ppm = "$dir\$name.ppm"
for ($i = 0; $i -lt 6; $i++) {
    ssh.exe -o BatchMode=yes -o ConnectTimeout=25 pve-node "printf 'screendump /tmp/ai-402.ppm\nquit\n' | sudo qm monitor 402 >/dev/null; sudo /usr/bin/cat /tmp/ai-402.ppm > /tmp/ai-402-copy.ppm" 2>$null
    if ($LASTEXITCODE -eq 0) { break }
    Start-Sleep -Seconds 8
}
for ($i = 0; $i -lt 6; $i++) {
    scp.exe -O -o BatchMode=yes -o ConnectTimeout=25 pve-node:/tmp/ai-402-copy.ppm $ppm 2>$null | Out-Null
    if (Test-Path $ppm) { break }
    Start-Sleep -Seconds 8
}
if (Test-Path $ppm) {
    python -c "from PIL import Image; Image.open(r'$ppm').save(r'$dir\$name.png')"
    Remove-Item $ppm
    "$dir\$name.png"
} else { "screenshot failed" }
