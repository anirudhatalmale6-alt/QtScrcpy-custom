"""
AniFelix Web Remote - Custom Button Editor
===========================================
Edit web_buttons.json to add/remove custom buttons for the web remote.
Place this file and web_buttons.json in the config/ folder next to AniFelix.exe.

Usage:
  python edit_buttons.py list              - Show current buttons
  python edit_buttons.py add               - Add a new button (interactive)
  python edit_buttons.py remove <index>    - Remove button by index number
  python edit_buttons.py reset             - Reset to default buttons

Button Types:
  action  - Built-in action (home, back, menu, lock, wake, volup, voldown, appswitch)
  keycode - Android keycode number (26=Power, 3=Home, 4=Back, 24=VolUp, 25=VolDown, etc.)
  shell   - ADB shell command (runs: adb -s <serial> shell <command>)
  url     - HTTP request to external URL ({serial} gets replaced with device serial)

Examples:
  {"label": "Cloud Link", "url": "http://myserver.com/api/link?device={serial}"}
  {"label": "Open App",   "shell": "am start -n com.example.app/.MainActivity"}
  {"label": "Screenshot",  "shell": "screencap -p /sdcard/screen.png"}
  {"label": "Vol+",       "action": "volup"}
  {"label": "Power Key",  "keycode": 26}
"""

import json
import os
import sys

CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web_buttons.json")

def load_buttons():
    if os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    return []

def save_buttons(buttons):
    with open(CONFIG_FILE, "w") as f:
        json.dump(buttons, f, indent=2)
    print(f"Saved {len(buttons)} button(s) to {CONFIG_FILE}")
    print("Refresh the web page to see changes.")

def list_buttons():
    buttons = load_buttons()
    if not buttons:
        print("No custom buttons configured.")
        return
    print(f"\n{'#':<4} {'Label':<15} {'Type':<10} {'Value'}")
    print("-" * 60)
    for i, btn in enumerate(buttons):
        if "action" in btn:
            print(f"{i:<4} {btn['label']:<15} {'action':<10} {btn['action']}")
        elif "keycode" in btn:
            print(f"{i:<4} {btn['label']:<15} {'keycode':<10} {btn['keycode']}")
        elif "shell" in btn:
            print(f"{i:<4} {btn['label']:<15} {'shell':<10} {btn['shell']}")
        elif "url" in btn:
            print(f"{i:<4} {btn['label']:<15} {'url':<10} {btn['url']}")
    print()

def add_button():
    print("\nButton types: action, keycode, shell, url")
    btn_type = input("Type: ").strip().lower()
    label = input("Label (button text): ").strip()
    if not label:
        print("Label is required.")
        return

    btn = {"label": label}
    if btn_type == "action":
        print("Actions: home, back, menu, lock, wake, volup, voldown, appswitch")
        btn["action"] = input("Action name: ").strip()
    elif btn_type == "keycode":
        print("Common: 3=Home, 4=Back, 24=VolUp, 25=VolDown, 26=Power, 82=Menu, 187=AppSwitch")
        btn["keycode"] = int(input("Keycode number: ").strip())
    elif btn_type == "shell":
        print("Example: am start -n com.package/.Activity")
        print("Use {serial} for device serial if needed")
        btn["shell"] = input("Shell command: ").strip()
    elif btn_type == "url":
        print("Use {serial} placeholder for device serial")
        print("Example: http://myserver.com/api/link?device={serial}")
        btn["url"] = input("URL: ").strip()
        method = input("HTTP method (GET/POST) [GET]: ").strip().upper()
        if method == "POST":
            btn["method"] = "POST"
    else:
        print(f"Unknown type: {btn_type}")
        return

    buttons = load_buttons()
    buttons.append(btn)
    save_buttons(buttons)

def remove_button(index):
    buttons = load_buttons()
    if index < 0 or index >= len(buttons):
        print(f"Invalid index {index}. Use 'list' to see buttons.")
        return
    removed = buttons.pop(index)
    save_buttons(buttons)
    print(f"Removed: {removed['label']}")

def reset_buttons():
    default = [
        {"label": "Vol+", "action": "volup"},
        {"label": "Vol-", "action": "voldown"},
        {"label": "Recent", "action": "appswitch"}
    ]
    save_buttons(default)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    cmd = sys.argv[1].lower()
    if cmd == "list":
        list_buttons()
    elif cmd == "add":
        add_button()
    elif cmd == "remove" and len(sys.argv) > 2:
        remove_button(int(sys.argv[2]))
    elif cmd == "reset":
        reset_buttons()
    else:
        print(__doc__)
