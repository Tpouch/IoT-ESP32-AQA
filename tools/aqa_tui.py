#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "textual>=8.2,<9",
#     "textual-plotext>=1.0,<2",
#     "pyserial>=3.5,<4",
#     "platformio>=6.1,<7",
# ]
# ///
"""Terminal dashboard for the AQA node: live sensor graphs, serial log, build & flash.

    uv run tools/aqa_tui.py                  # autodetect the board's serial port
    uv run tools/aqa_tui.py --port /dev/ttyUSB0
    uv run tools/aqa_tui.py --demo           # simulated data, no board needed

uv installs the dependencies (Textual, plotext, pyserial, PlatformIO) in a
cached environment on first run. Data comes from the serial log: the firmware
prints every 15 s frame as
    [  123.456] [INFO]  frame queued (3 pending): {"t":..,"age_s":0,"snd":..,"co2":..,..}
so nothing changes on the device side.
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import json
import math
import os
import random
import re
import shutil
import sys
import threading
import time
from pathlib import Path

import serial
from rich.text import Text
from serial.tools import list_ports
from textual import on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.reactive import reactive
from textual.widgets import (Button, Footer, Header, RichLog, Sparkline, Static,
                             TabbedContent, TabPane)
from textual_plotext import PlotextPlot

PROJECT_DIR = Path(__file__).resolve().parent.parent
PIO_ENV = "ttgo-t7-v14-mini32"
BAUD = 115200
HISTORY = 2000  # frames kept per metric (~8 h at one frame every 15 s)

# JSON key, label, unit, decimals, colour
METRICS = [
    ("co2", "CO2", "ppm", 0, "cyan"),
    ("pm25", "PM2.5", "µg/m³", 0, "magenta"),
    ("snd", "Son", "p2p", 0, "blue"),
    ("temp", "Température", "°C", 1, "yellow"),
    ("hum", "Humidité", "%", 1, "green"),
]

LEVEL_STYLE = {"ERROR": "bold red", "WARN": "yellow", "DEBUG": "dim", "VERB": "dim"}
LEVEL_RE = re.compile(r"\[(ERROR|WARN|INFO|DEBUG|VERB)\]")


# --------------------------------------------------------------------------- helpers

def find_pio(explicit: str | None) -> str | None:
    candidates = [
        explicit,
        os.environ.get("PIO"),
        shutil.which("pio"),
        shutil.which("platformio"),
        str(Path.home() / ".platformio/penv/bin/pio"),
        # The PlatformIO installed by uv next to this script's interpreter.
        str(Path(sys.executable).parent / "pio"),
    ]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def board_ports() -> list[str]:
    ports = []
    for p in list_ports.comports():
        if p.vid is not None or re.search(r"ttyUSB|ttyACM|usbserial|SLAB", p.device):
            ports.append(p.device)
    return sorted(ports)


def fmt(value, decimals: int) -> str:
    return "—" if value is None else f"{value:.{decimals}f}"


def age(seconds: float) -> str:
    if seconds < 90:
        return f"{int(seconds)} s"
    if seconds < 5400:
        return f"{int(seconds / 60)} min"
    return f"{seconds / 3600:.1f} h"


def styled_log_line(line: str) -> Text:
    if line.startswith("---"):
        return Text(line, style="bold cyan")
    m = LEVEL_RE.search(line)
    return Text(line, style=LEVEL_STYLE.get(m.group(1), "") if m else "")


def styled_build_line(line: str) -> Text:
    low = line.lower()
    if "[success]" in low:
        return Text(line, style="bold green")
    if "error" in low or "[failed]" in low or "failed" in low:
        return Text(line, style="red")
    if "warning" in low:
        return Text(line, style="yellow")
    if line.startswith("$ "):
        return Text(line, style="bold")
    return Text(line)


# --------------------------------------------------------------------------- data

class Telemetry:
    """Parses serial lines; keeps per-metric history and board info."""

    def __init__(self) -> None:
        self.history = {m[0]: collections.deque(maxlen=HISTORY) for m in METRICS}
        self.times: collections.deque[float] = collections.deque(maxlen=HISTORY)
        self.info: dict[str, str] = {}
        self.stale = False
        self.last_frame_at: float | None = None

    def ingest(self, line: str) -> bool:
        """Returns True when the line was a telemetry frame."""
        if "frame queued" in line or "frame sent to" in line or "frame dropped" in line:
            brace = line.find("{")
            if brace == -1:
                return False
            try:
                frame = json.loads(line[brace:])
            except ValueError:
                return False  # truncated line
            if "fields" in frame:  # firmware 1.x: [{"snd":..},{"co2":..},..]
                values = {}
                for field in frame["fields"]:
                    values.update(field)
            else:                  # firmware 2.x: flat object, null = no reading
                values = frame
            now = time.time()
            for key, *_ in METRICS:
                # Keep the series aligned with `times`: a missing field is None.
                self.history[key].append(values.get(key))
            self.times.append(now)
            self.last_frame_at = now
            self.stale = bool(frame.get("stale"))
            if m := re.search(r"frame queued \((\d+) pending\)", line):
                self.info["queue"] = m.group(1)
            return True

        if m := re.search(r"firmware (\S+) on", line):
            self.info["fw"] = m.group(1)
        if m := re.search(r"CO2 sensor detected: (.+)$", line):
            self.info["co2"] = m.group(1).strip()
        if "self-test |" in line:
            self.info["selftest"] = line.split("self-test |", 1)[1].strip()
        if m := re.search(r"wifi ip=(\S+) rssi=(-?\d+) dBm api=(.+?) queue=(\d+)", line):
            self.info["wifi"] = f"{m.group(1)} · {m.group(2)} dBm"
            self.info["api"] = m.group(3)
            self.info["queue"] = m.group(4)
        elif m := re.search(r"wifi ip=(\S+) rssi=(-?\d+) dBm mqtt=(\w+)", line):  # firmware 1.x
            self.info["wifi"] = f"{m.group(1)} · {m.group(2)} dBm · mqtt {m.group(3)}"
        elif m := re.search(r"wifi offline, queue=(\d+)", line):
            self.info["wifi"] = "hors ligne"
            self.info["queue"] = m.group(1)
        elif "wifi offline" in line:
            self.info["wifi"] = "hors ligne"
        if m := re.search(r"api: (\d+) frame\(s\) uploaded, (\d+) pending", line):
            self.info["api"] = "ok"
            self.info["queue"] = m.group(2)
        elif "api: token refused" in line:
            self.info["api"] = "token refusé"
        elif re.search(r"api: POST \S+ failed", line):
            self.info["api"] = "injoignable"
        return False

    def series(self, key: str) -> tuple[list[float], list[float]]:
        xs, ys = [], []
        for t, v in zip(self.times, self.history[key]):
            if v is not None:
                xs.append(t)
                ys.append(v)
        return xs, ys

    def save_csv(self, path: Path) -> None:
        keys = [m[0] for m in METRICS]
        with path.open("w", encoding="utf-8") as fh:
            fh.write("time," + ",".join(keys) + "\n")
            for i, t in enumerate(self.times):
                row = ["" if self.history[k][i] is None else str(self.history[k][i]) for k in keys]
                stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t))
                fh.write(stamp + "," + ",".join(row) + "\n")


class DemoBoard:
    """Fake serial source for --demo."""

    def __init__(self) -> None:
        self.t0 = time.time()
        self.k = 0
        self.queue = collections.deque([
            "[    0.120] [INFO]  firmware 1.0.0 on ttgo-t7-v14-mini32 (démo)",
            "[    3.010] [INFO]  CO2 sensor detected: MH-Z19",
            "[    9.400] [INFO]  self-test | temperature PASS | co2 PASS | pm PASS | sound PASS",
        ])

    def lines(self):
        while self.queue:
            yield self.queue.popleft()
        self.k += 1
        k = self.k
        frame = {
            "t": int(time.time()), "age_s": 0,
            "snd": int(40 + 25 * abs(math.sin(k / 7)) + random.randint(0, 15)),
            "co2": int(520 + 180 * math.sin(k / 25) + random.randint(-15, 15)),
            "pm25": max(0, int(12 + 8 * math.sin(k / 11) + random.randint(-3, 3))),
            "hum": round(48 + 6 * math.sin(k / 40) + random.random(), 1),
            "temp": round(22.5 + 1.5 * math.sin(k / 50) + random.random() / 3, 1),
            "stale": False,
        }
        ts = time.time() - self.t0
        yield (f"[{ts:9.3f}] [INFO]  frame queued (1 pending): "
               + json.dumps(frame, separators=(",", ":")))
        yield f"[{ts:9.3f}] [INFO]  api: 1 frame(s) uploaded, 0 pending"
        if k % 5 == 0:
            yield f"[{ts:9.3f}] [INFO]  wifi ip=192.168.1.42 rssi=-6{k % 10} dBm api=ok queue=0"


# --------------------------------------------------------------------------- widgets

class MetricCard(Vertical, can_focus=False):
    """Current value + sparkline. Click to show it in the big chart."""

    selected = reactive(False)

    def __init__(self, index: int) -> None:
        key, label, unit, dec, colour = METRICS[index]
        super().__init__(id=f"card-{key}", classes="card")
        self.index, self.key, self.label_text = index, key, label
        self.unit, self.dec, self.colour = unit, dec, colour

    def compose(self) -> ComposeResult:
        yield Static(self.label_text, classes="card-title")
        yield Static("—", classes="card-value")
        yield Sparkline([], classes="card-spark")

    def on_mount(self) -> None:
        self.query_one(".card-title", Static).styles.color = self.colour
        spark = self.query_one(Sparkline)
        spark.styles.color = self.colour

    def update_values(self, values: list[float]) -> None:
        value = values[-1] if values else None
        self.query_one(".card-value", Static).update(
            Text.assemble((fmt(value, self.dec), f"bold {self.colour}"), " ", (self.unit, "dim")))
        self.query_one(Sparkline).data = values[-60:]

    def watch_selected(self, selected: bool) -> None:
        self.set_class(selected, "selected")

    def on_click(self) -> None:
        self.app.select_metric(self.index)


class Chart(PlotextPlot):
    def show(self, label: str, unit: str, colour: str, xs: list[float], ys: list[float]) -> None:
        plt = self.plt
        plt.clear_data()
        plt.clear_figure()
        plt.title(f"{label} ({unit})")
        plt.xlabel("minutes")
        if ys:
            now = time.time()
            plt.plot([(x - now) / 60 for x in xs], ys, color=colour, marker="braille")
        self.refresh()


# --------------------------------------------------------------------------- app

class AqaMonitor(App):
    TITLE = "AQA monitor"
    CSS = """
    #status { height: auto; padding: 0 1; }
    #cards { height: 6; }
    .card { width: 1fr; border: round $panel; padding: 0 1; }
    .card.selected { border: round $accent; }
    .card-title { text-style: bold; }
    .card-spark { height: 2; }
    #chart { height: 1fr; min-height: 8; }
    #build-info { height: auto; padding: 0 1; }
    #build-buttons { height: 3; }
    #build-buttons Button { margin-right: 1; }
    RichLog { border: round $panel; }
    """
    BINDINGS = [
        Binding("q", "quit", "Quitter"),
        Binding("1", "tab('sensors')", "Capteurs"),
        Binding("2", "tab('log')", "Log"),
        Binding("3", "tab('build')", "Build"),
        # Priority so the focused tab bar does not swallow the arrows; only
        # active on the sensors tab (see check_action), elsewhere they fall
        # through to the focused widget.
        Binding("left", "metric(-1)", "◀ capteur", priority=True),
        Binding("right", "metric(1)", "capteur ▶", priority=True),
        Binding("b", "build", "Build"),
        Binding("f", "flash", "Flash"),
        Binding("r", "reset_board", "Reset carte"),
        Binding("s", "save_csv", "CSV"),
    ]

    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__()
        self.args = args
        self.telemetry = Telemetry()
        self.metric = 0
        self.pio = find_pio(args.pio)
        self.port: str | None = None
        self.port_status = "recherche du port…"
        self.serial: serial.Serial | None = None
        self.serial_lock = threading.Lock()
        self.release_port = threading.Event()  # set while esptool needs the port
        self.stop_event = threading.Event()
        self.build_proc: asyncio.subprocess.Process | None = None
        self.build_label = ""
        self.build_result = ""

    # ---- layout

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with TabbedContent(initial="sensors"):
            with TabPane("Capteurs", id="sensors"):
                yield Static(id="status")
                with Horizontal(id="cards"):
                    for i in range(len(METRICS)):
                        yield MetricCard(i)
                yield Chart(id="chart")
            with TabPane("Log série", id="log"):
                yield RichLog(id="serial-log", max_lines=5000, wrap=False, markup=False)
            with TabPane("Build & Flash", id="build"):
                yield Static(id="build-info")
                with Horizontal(id="build-buttons"):
                    yield Button("Build", id="btn-build", variant="primary")
                    yield Button("Flash", id="btn-flash", variant="success")
                    yield Button("Clean", id="btn-clean")
                    yield Button("Stop", id="btn-stop", variant="error", disabled=True)
                yield RichLog(id="build-log", max_lines=5000, wrap=False, markup=False)
        yield Footer()

    def on_mount(self) -> None:
        self.select_metric(0)
        self.refresh_build_info()
        self.set_interval(1.0, self.refresh_status)
        self.read_serial()

    def on_unmount(self) -> None:
        self.stop_event.set()
        if self.build_proc and self.build_proc.returncode is None:
            self.build_proc.terminate()

    # ---- serial (background thread)

    @work(thread=True, exclusive=True, group="serial")
    def read_serial(self) -> None:
        if self.args.demo:
            self.port_status = "démo"
            demo = DemoBoard()
            while not self.stop_event.is_set():
                for line in demo.lines():
                    self.call_from_thread(self.handle_line, line)
                self.stop_event.wait(1.0)
            return

        buf = b""
        while not self.stop_event.is_set():
            if self.release_port.is_set():
                self.stop_event.wait(0.2)
                continue
            if self.serial is None:
                self.open_port()
                if self.serial is None:
                    self.stop_event.wait(2.0)
                    continue
            try:
                with self.serial_lock:
                    chunk = self.serial.read(4096) if self.serial else b""
            except (serial.SerialException, OSError) as exc:
                self.call_from_thread(self.handle_line, f"--- port perdu : {exc} ---")
                self.close_port()
                self.port_status = "port perdu, nouvelle tentative…"
                continue
            if not chunk:
                continue
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                self.call_from_thread(self.handle_line, raw.decode("utf-8", "replace").rstrip("\r"))

    def open_port(self) -> None:
        ports = [self.args.port] if self.args.port else board_ports()
        if not ports:
            self.port_status = "aucune carte détectée (branche-la en USB)"
            return
        path = ports[0]
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = path, BAUD, 0.2
        # Released DTR/RTS = "run" on the ESP32 auto-reset circuit, so opening
        # the port does not hold the board in reset or in the bootloader.
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
        except serial.SerialException as exc:
            if "Permission denied" in str(exc):
                self.port_status = f"{path} : permission refusée → sudo usermod -aG dialout $USER puis reconnecte-toi"
            else:
                self.port_status = f"{path} : {exc}"
            return
        with self.serial_lock:
            self.serial = ser
        self.port = path
        self.port_status = path
        self.call_from_thread(self.handle_line, f"--- {path} ouvert à {BAUD} bauds ---")

    def close_port(self) -> None:
        with self.serial_lock:
            if self.serial:
                try:
                    self.serial.close()
                except Exception:
                    pass
            self.serial = None

    def handle_line(self, line: str) -> None:
        self.query_one("#serial-log", RichLog).write(styled_log_line(line))
        if self.telemetry.ingest(line):
            self.refresh_metrics()
        self.refresh_status()

    # ---- sensors tab

    def refresh_metrics(self) -> None:
        for card in self.query(MetricCard):
            card.update_values(self.telemetry.series(card.key)[1])
        self.refresh_chart()

    def refresh_chart(self) -> None:
        key, label, unit, _, colour = METRICS[self.metric]
        xs, ys = self.telemetry.series(key)
        self.query_one(Chart).show(label, unit, colour, xs, ys)

    def select_metric(self, index: int) -> None:
        self.metric = index % len(METRICS)
        for card in self.query(MetricCard):
            card.selected = card.index == self.metric
        self.refresh_chart()

    def refresh_status(self) -> None:
        t = self.telemetry
        ok = self.serial is not None or self.args.demo
        parts = [Text("● ", style="green" if ok else "red"), Text(self.port_status)]
        for label, key in (("fw", "fw"), ("CO2", "co2"), ("wifi", "wifi"),
                           ("api", "api"), ("file", "queue")):
            if key in t.info:
                parts += [Text("   " + label + " ", style="dim"), Text(t.info[key])]
        if t.last_frame_at:
            parts.append(Text(f"   dernière trame il y a {age(time.time() - t.last_frame_at)}", style="dim"))
        line = Text.assemble(*parts)
        if "selftest" in t.info:
            line.append("\nself-test  ", style="dim")
            for chunk in t.info["selftest"].split("|"):
                chunk = chunk.strip()
                line.append(chunk + "   ", style="green" if chunk.endswith("PASS") else "bold red")
        if t.stale:
            line.append("\n⚠ valeurs identiques sur 3 trames (stale)", style="bold yellow")
        self.query_one("#status", Static).update(line)

    # ---- build tab

    def refresh_build_info(self) -> None:
        env_path = PROJECT_DIR / ".env"
        info = Text()
        info.append("pio   ", style="dim")
        info.append(self.pio or "introuvable : lance le script avec `uv run` ou passe --pio",
                    style="" if self.pio else "bold red")
        info.append("\n.env  ", style="dim")
        if env_path.is_file():
            keys = [ln.split("=", 1)[0].strip() for ln in env_path.read_text(encoding="utf-8").splitlines()
                    if ln.strip() and not ln.lstrip().startswith("#") and "=" in ln]
            info.append("présent (" + ", ".join(keys) + ")", style="green")
        else:
            info.append("absent : valeurs par défaut de secrets.h (cp .env.example .env)", style="yellow")
        if self.build_result:
            info.append("\n" + self.build_result,
                        style="bold green" if "OK" in self.build_result else "bold red")
        elif self.build_label:
            info.append(f"\n{self.build_label} en cours…", style="bold yellow")
        self.query_one("#build-info", Static).update(info)

    def set_running(self, running: bool) -> None:
        for bid in ("#btn-build", "#btn-flash", "#btn-clean"):
            self.query_one(bid, Button).disabled = running
        self.query_one("#btn-stop", Button).disabled = not running

    @work(exclusive=True, group="build")
    async def run_pio(self, label: str, extra: list[str]) -> None:
        log = self.query_one("#build-log", RichLog)
        self.query_one(TabbedContent).active = "build"
        if not self.pio:
            self.notify("pio introuvable", severity="error")
            return
        args = [self.pio, "run", "-e", PIO_ENV, *extra]
        flashing = "upload" in extra
        if flashing:
            if self.args.demo:
                self.notify("Flash désactivé en mode démo", severity="warning")
                return
            port = self.port or self.args.port
            if port:
                args += ["--upload-port", port]
            self.release_port.set()  # esptool needs exclusive access
            await asyncio.sleep(0.3)
            self.close_port()

        log.clear()
        log.write(Text("$ " + " ".join(args), style="bold"))
        self.build_label, self.build_result = label, ""
        self.set_running(True)
        self.refresh_build_info()
        started = time.time()
        try:
            self.build_proc = await asyncio.create_subprocess_exec(
                *args, cwd=PROJECT_DIR, stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
            assert self.build_proc.stdout
            async for raw in self.build_proc.stdout:
                log.write(styled_build_line(raw.decode("utf-8", "replace").rstrip()))
            code = await self.build_proc.wait()
        except OSError as exc:
            log.write(Text(f"échec du lancement : {exc}", style="bold red"))
            code = -1
        finally:
            self.build_proc = None
            if flashing:
                self.release_port.clear()
            self.set_running(False)
        ok = code == 0
        self.build_result = f"{label} : {'OK ✔' if ok else f'ÉCHEC ✘ (code {code})'}  en {time.time() - started:.0f} s"
        self.build_label = ""
        self.refresh_build_info()
        self.notify(self.build_result, severity="information" if ok else "error")

    @on(Button.Pressed, "#btn-build")
    def action_build(self) -> None:
        self.run_pio("build", [])

    @on(Button.Pressed, "#btn-flash")
    def action_flash(self) -> None:
        self.run_pio("flash", ["-t", "upload"])

    @on(Button.Pressed, "#btn-clean")
    def on_clean(self) -> None:
        self.run_pio("clean", ["-t", "clean"])

    @on(Button.Pressed, "#btn-stop")
    def on_stop(self) -> None:
        if self.build_proc and self.build_proc.returncode is None:
            self.build_proc.terminate()
            self.notify("commande interrompue", severity="warning")

    # ---- other actions

    def check_action(self, action: str, parameters: tuple) -> bool | None:
        if action == "metric":
            try:
                return self.query_one(TabbedContent).active == "sensors"
            except Exception:
                return False
        return True

    def action_tab(self, tab: str) -> None:
        self.query_one(TabbedContent).active = tab

    def action_metric(self, step: int) -> None:
        self.select_metric(self.metric + step)

    def action_reset_board(self) -> None:
        with self.serial_lock:
            ser = self.serial
            if ser is None:
                self.notify("pas de carte connectée", severity="warning")
                return
            # RTS pulls EN low through the auto-reset transistor; DTR stays
            # released so GPIO0 is high and the application boots.
            try:
                ser.dtr = False
                ser.rts = True
                time.sleep(0.12)
                ser.rts = False
            except (serial.SerialException, OSError) as exc:
                self.notify(f"reset impossible sur ce port : {exc}", severity="error")
                return
        self.handle_line("--- reset de la carte ---")

    def action_save_csv(self) -> None:
        if not self.telemetry.times:
            self.notify("aucune donnée à sauvegarder", severity="warning")
            return
        path = PROJECT_DIR / time.strftime("aqa-%Y%m%d-%H%M%S.csv")
        self.telemetry.save_csv(path)
        self.notify(f"sauvegardé : {path.name}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Dashboard TUI du capteur AQA : graphes, log série, build et flash.")
    parser.add_argument("--port", help="port série (défaut : première carte USB détectée)")
    parser.add_argument("--pio", help="exécutable pio (défaut : PATH, $PIO, ~/.platformio/penv, ou celui installé par uv)")
    parser.add_argument("--demo", action="store_true", help="données simulées, sans carte")
    AqaMonitor(parser.parse_args()).run()


if __name__ == "__main__":
    main()
