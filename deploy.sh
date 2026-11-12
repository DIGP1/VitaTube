#!/usr/bin/env bash
set -euo pipefail

# === REGISTRO DE LOGS ===
exec > >(tee -a deploy.log) 2>&1

cd /workspace

VITA_IP="${VITA_IP:-192.168.31.12}"
TITLEID="${TITLEID:-VTTB00001}"

echo "==> Compilando VitaTube..."
cmake --build build -j2

SELF="$(find build -type f -name 'VitaTube.self' -print -quit)"

if [ -z "$SELF" ]; then
    echo "ERROR: No se encontró VitaTube.self."
    echo "Archivos disponibles:"
    find build -type f \( -name '*.self' -o -name '*.vpk' -o -name 'eboot.bin' \)
    exit 1
fi

echo "==> Cerrando VitaTube en la PS Vita si está abierta..."
(printf "quit $TITLEID\n" > /dev/tcp/"$VITA_IP"/1338) 2>/dev/null || true
sleep 2

echo "==> Transfiriendo ejecutable..."
python3 - <<EOF
import ftplib, sys
ip = "$VITA_IP"
self_path = "$SELF"
title_id = "$TITLEID"
for attempt in range(1, 4):
    try:
        ftp = ftplib.FTP()
        ftp.connect(ip, 1337, timeout=10)
        ftp.login()
        with open(self_path, 'rb') as f:
            ftp.storbinary(f'STOR /ux0:/app/{title_id}/eboot.bin', f)
        assets = [
            ('assets/icon0.png', f'/ux0:/app/{title_id}/sce_sys/icon0.png', f'/ur0:/appmeta/{title_id}/icon0.png'),
            ('assets/livearea/contents/bg.png', f'/ux0:/app/{title_id}/sce_sys/livearea/contents/bg.png', f'/ur0:/appmeta/{title_id}/livearea/contents/bg.png'),
            ('assets/livearea/contents/startup.png', f'/ux0:/app/{title_id}/sce_sys/livearea/contents/startup.png', f'/ur0:/appmeta/{title_id}/livearea/contents/startup.png'),
            ('assets/livearea/contents/template.xml', f'/ux0:/app/{title_id}/sce_sys/livearea/contents/template.xml', f'/ur0:/appmeta/{title_id}/livearea/contents/template.xml'),
        ]
        for local_path, app_dest, meta_dest in assets:
            for dest in (app_dest, meta_dest):
                try:
                    with open(local_path, 'rb') as af:
                        ftp.storbinary(f'STOR {dest}', af)
                except Exception:
                    pass
        ftp.quit()
        print("Transferencia FTP exitosa.")
        sys.exit(0)
    except Exception as e:
        print(f"Error en intento {attempt}: {e}")
        import time; time.sleep(2)
sys.exit(1)
EOF

echo "==> Transferencia completada."
echo "Pendiente: lanzar VitaTube con VitaCompanion."

echo "==> Lanzando VitaTube en la PS Vita..."
printf 'launch VTTB00001\n' > /dev/tcp/"$VITA_IP"/1338
echo "==> Comando de lanzamiento enviado."
