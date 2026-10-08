#!/usr/bin/env bash
# Tür per Desktop-Symbol öffnen (Linux).
#
# Einrichten - legt "Tür öffnen" im Anwendungsmenü und auf dem Desktop an:
#   bash tuer-oeffnen.sh --install --address 192.168.20.50
#
# Ein Klick sendet POST /open an den Türöffner. Ist ein Tür-Passwort gesetzt, fragt
# das Skript beim ersten Mal nach Benutzer und Passwort und speichert sie im
# Schlüsselbund des Desktops (secret-tool), sonst in einer nur für diesen Benutzer
# lesbaren Datei. Am besten einen eigenen Benutzer anlegen (Rolle Tür), nie den Admin.
# --reset vergisst die gespeicherte Anmeldung.
# Braucht curl; Fenster und Meldungen über zenity, kdialog oder notify-send (optional).
set -u

address="tueroeffner.local"
install=0
reset=0
while [ $# -gt 0 ]; do
  case "$1" in
    --install)   install=1 ;;
    --reset)     reset=1 ;;
    --address)   shift; address="${1:-}" ;;
    --address=*) address="${1#*=}" ;;
    -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
    *)           echo "Unbekannte Option: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$address" ] || { echo "Adresse fehlt (--address)" >&2; exit 2; }

have() { command -v "$1" >/dev/null 2>&1; }

# Rückmeldung: Erfolg kurz als Benachrichtigung, Fehler als Fenster (immer auch Terminal)
info() {
  if have notify-send; then notify-send -i changes-allow -t 3000 "Türöffner" "$1" 2>/dev/null
  elif have zenity;    then zenity --info --title="Türöffner" --text="$1" --timeout=3 2>/dev/null
  elif have kdialog;   then kdialog --title "Türöffner" --passivepopup "$1" 3 2>/dev/null
  fi
  echo "$1"
}
error() {
  if have zenity;        then zenity --error --title="Türöffner" --text="$1" 2>/dev/null
  elif have kdialog;     then kdialog --title "Türöffner" --error "$1" 2>/dev/null
  elif have notify-send; then notify-send -u critical -i dialog-error "Türöffner" "$1" 2>/dev/null
  fi
  echo "$1" >&2
}

if [ "$install" = 1 ]; then
  # Skript an festen Ort kopieren, damit der Starter nicht vom Download-Ordner abhängt
  data="${XDG_DATA_HOME:-$HOME/.local/share}"
  target="$data/tueroeffner/tuer-oeffnen.sh"
  mkdir -p "$data/tueroeffner" "$data/applications"
  src=$(readlink -f "$0")
  [ "$src" = "$(readlink -f "$target" 2>/dev/null)" ] || cp "$src" "$target"
  chmod 755 "$target"
  entry="$data/applications/tueroeffner.desktop"
  cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=Tür öffnen
Comment=Türöffner ($address)
Exec="$target" --address $address
Icon=changes-allow
Terminal=false
Categories=Utility;
EOF
  chmod 755 "$entry"
  have update-desktop-database && update-desktop-database "$data/applications" 2>/dev/null
  where="Anwendungsmenü"
  # Kopie auf den Desktop (Ordnername je nach Sprache, z.B. "Schreibtisch")
  desktop=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
  if [ -d "$desktop" ] && [ "$desktop" != "$HOME" ]; then
    cp "$entry" "$desktop/tueroeffner.desktop"
    chmod 755 "$desktop/tueroeffner.desktop"
    # GNOME: als vertrauenswürdig markieren (sonst erst "Start erlauben" per Rechtsklick)
    have gio && gio set "$desktop/tueroeffner.desktop" metadata::trusted true 2>/dev/null
    where="$where und Desktop"
  fi
  echo "\"Tür öffnen\" angelegt ($address): $where."
  exit 0
fi

# Gespeicherte Anmeldung "benutzer:passwort": Schlüsselbund, sonst Datei (nur lesbar
# für diesen Benutzer)
cfgdir="${XDG_CONFIG_HOME:-$HOME/.config}/tueroeffner"
credfile="$cfgdir/zugang-$(printf '%s' "$address" | tr -c 'A-Za-z0-9.-' '_')"

load_cred() {
  if have secret-tool && secret-tool lookup service tueroeffner address "$address" 2>/dev/null; then
    return
  fi
  if [ -r "$credfile" ]; then cat "$credfile"; fi
}
save_cred() {
  if have secret-tool && printf '%s' "$1" |
       secret-tool store --label="Türöffner $address" service tueroeffner address "$address" 2>/dev/null; then
    return
  fi
  mkdir -p "$cfgdir" && (umask 077 && printf '%s' "$1" > "$credfile")
}
forget_cred() {
  if have secret-tool; then secret-tool clear service tueroeffner address "$address" 2>/dev/null; fi
  rm -f "$credfile"
}

# Anmeldung abfragen; 1 = abgebrochen, 2 = kein Fenster und kein Terminal
ask_cred() {
  local r u p
  if have zenity; then
    r=$(zenity --password --username --title="Türöffner $address" 2>/dev/null) || return 1
    printf '%s:%s' "${r%%|*}" "${r#*|}"   # zenity liefert "benutzer|passwort"
  elif have kdialog; then
    u=$(kdialog --title "Türöffner $address" --inputbox "Benutzer:") || return 1
    p=$(kdialog --title "Türöffner $address" --password "Passwort für $u:") || return 1
    printf '%s:%s' "$u" "$p"
  elif [ -t 0 ]; then
    read -r -p "Benutzer für den Türöffner ($address): " u || return 1
    read -r -s -p "Passwort: " p || return 1
    echo >&2
    printf '%s:%s' "$u" "$p"
  else
    return 2
  fi
}

# POST /open; setzt code (HTTP-Status, 000 = nicht erreichbar) und msg (Fehlertext)
open_door() {
  local body esc
  body=$(mktemp) || exit 1
  if [ -n "$1" ]; then
    # Anmeldung per stdin an curl - nicht in der Prozessliste sichtbar
    esc=${1//\\/\\\\}
    esc=${esc//\"/\\\"}
    code=$(printf 'user = "%s"\n' "$esc" |
           curl -s -o "$body" -w '%{http_code}' --max-time 5 -X POST -K - "http://$address/open")
  else
    code=$(curl -s -o "$body" -w '%{http_code}' --max-time 5 -X POST "http://$address/open" </dev/null)
  fi
  msg=$(sed -n 's/.*"err":"\([^"]*\)".*/\1/p' "$body")
  rm -f "$body"
}

if [ "$reset" = 1 ]; then
  forget_cred
  info "Gespeicherte Anmeldung gelöscht."
  exit 0
fi
have curl || { error "curl fehlt (z. B. sudo apt install curl)"; exit 1; }

cred=$(load_cred)
open_door "$cred"
if [ "$code" = 401 ]; then
  # Tür-Passwort gesetzt (oder gespeicherte Anmeldung falsch): nachfragen und merken
  cred=$(ask_cred)
  case $? in
    0) ;;
    2) error "Anmeldung nötig, aber kein Fenster möglich (zenity oder kdialog installieren)"; exit 1 ;;
    *) exit 0 ;;   # abgebrochen
  esac
  open_door "$cred"
  [ "$code" = 200 ] && save_cred "$cred"
fi

case "$code" in
  200) info "Tür wird geöffnet." ;;
  000) error "Türöffner nicht erreichbar ($address)."; exit 1 ;;
  *)   error "Nicht geöffnet: ${msg:-HTTP $code}"; exit 1 ;;
esac
