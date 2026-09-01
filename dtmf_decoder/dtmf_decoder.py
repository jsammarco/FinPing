import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import sounddevice as sd
import numpy as np
import threading
import queue
import time
import json
import ctypes
import csv
import os
from pathlib import Path
import shlex
import subprocess
import sys
from datetime import datetime


# ============================================================
# DTMF CONFIGURATION
# ============================================================

DTMF_LOW = [697, 770, 852, 941]
DTMF_HIGH = [1209, 1336, 1477, 1633]

DTMF_MAP = {
    (697, 1209): "1",
    (697, 1336): "2",
    (697, 1477): "3",
    (697, 1633): "A",

    (770, 1209): "4",
    (770, 1336): "5",
    (770, 1477): "6",
    (770, 1633): "B",

    (852, 1209): "7",
    (852, 1336): "8",
    (852, 1477): "9",
    (852, 1633): "C",

    (941, 1209): "*",
    (941, 1336): "0",
    (941, 1477): "#",
    (941, 1633): "D",
}


SETTINGS_FILE = Path(__file__).with_name("dtmf_decoder_settings.json")
SEQUENCE_GROUP_DELAY_MS = 3000

ACTION_TYPE_PROGRAM = "program"
ACTION_TYPE_SOUND = "sound"

ACTION_TYPE_LABELS = {
    ACTION_TYPE_PROGRAM: "Run program",
    ACTION_TYPE_SOUND: "Play sound",
}


class DTMFDetector:
    """
    DTMF detector using the Goertzel algorithm.
    """

    def __init__(self, sample_rate=48000):
        self.sample_rate = sample_rate

        # Detection tuning
        self.minimum_rms = 0.004
        self.relative_threshold = 3.0
        self.dominance_ratio = 2.0

    def goertzel_power(self, samples, frequency):
        """
        Calculate power at one frequency using the Goertzel algorithm.
        """

        n = len(samples)

        if n == 0:
            return 0.0

        k = int(0.5 + ((n * frequency) / self.sample_rate))
        omega = (2.0 * np.pi * k) / n

        cosine = np.cos(omega)
        sine = np.sin(omega)
        coeff = 2.0 * cosine

        q0 = 0.0
        q1 = 0.0
        q2 = 0.0

        for sample in samples:
            q0 = coeff * q1 - q2 + sample
            q2 = q1
            q1 = q0

        real = q1 - q2 * cosine
        imag = q2 * sine

        return real * real + imag * imag

    def detect(self, samples):
        """
        Returns:
            (key, low_frequency, high_frequency, confidence)

        or:
            (None, None, None, 0)
        """

        samples = np.asarray(samples, dtype=np.float32)

        if len(samples) < 100:
            return None, None, None, 0.0

        # Remove DC offset
        samples = samples - np.mean(samples)

        rms = np.sqrt(np.mean(samples * samples))

        if rms < self.minimum_rms:
            return None, None, None, 0.0

        # Windowing makes detection a little cleaner
        samples = samples * np.hanning(len(samples))

        low_powers = {
            freq: self.goertzel_power(samples, freq)
            for freq in DTMF_LOW
        }

        high_powers = {
            freq: self.goertzel_power(samples, freq)
            for freq in DTMF_HIGH
        }

        low_sorted = sorted(
            low_powers.items(),
            key=lambda x: x[1],
            reverse=True
        )

        high_sorted = sorted(
            high_powers.items(),
            key=lambda x: x[1],
            reverse=True
        )

        low_freq, low_power = low_sorted[0]
        high_freq, high_power = high_sorted[0]

        second_low_power = low_sorted[1][1]
        second_high_power = high_sorted[1][1]

        # The winning frequencies should clearly dominate the
        # other candidates in their group.
        low_ratio = low_power / max(second_low_power, 1e-12)
        high_ratio = high_power / max(second_high_power, 1e-12)

        if low_ratio < self.dominance_ratio:
            return None, None, None, 0.0

        if high_ratio < self.dominance_ratio:
            return None, None, None, 0.0

        key = DTMF_MAP.get((low_freq, high_freq))

        if key is None:
            return None, None, None, 0.0

        confidence = min(low_ratio, high_ratio)

        return key, low_freq, high_freq, confidence


class DTMFDecoderGUI:
    def __init__(self, root):
        self.root = root

        self.root.title("DTMF Tone Decoder")
        self.root.geometry("1000x700")
        self.root.minsize(850, 600)

        # Persisted settings are loaded before the device and actions UI is
        # populated so both can reflect the last saved configuration.
        self.selected_device_setting = None
        self.actions = []
        self.detection_history = []
        self.load_settings()

        # ----------------------------------------------------
        # Audio
        # ----------------------------------------------------

        self.audio_queue = queue.Queue()
        self.event_queue = queue.Queue()

        self.stream = None
        self.running = False
        self.worker_thread = None
        self.active_sound_channels = []

        self.sample_rate = 48000
        self.block_size = 960  # 20 ms at 48 kHz

        self.detector = DTMFDetector(self.sample_rate)

        # ----------------------------------------------------
        # Tone state
        # ----------------------------------------------------

        self.current_candidate = None
        self.candidate_start = None

        self.active_tone = None
        self.active_start = None

        self.last_detected_time = None

        self.decoded_sequence = ""
        self.pending_sequence = None
        self.sequence_group_timer = None

        # Default 50 ms
        self.min_tone_ms = tk.IntVar(value=50)

        # ----------------------------------------------------
        # Build GUI
        # ----------------------------------------------------

        self.build_gui()
        self.refresh_devices()
        self.refresh_detection_tree()
        self.refresh_actions_tree()

        # Poll events generated by audio thread
        self.root.after(25, self.process_events)

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ========================================================
    # SETTINGS
    # ========================================================

    def load_settings(self):
        """Load the saved microphone identity and configured actions."""

        try:
            with SETTINGS_FILE.open("r", encoding="utf-8") as settings_file:
                settings = json.load(settings_file)
        except (OSError, json.JSONDecodeError):
            settings = {}

        saved_device = settings.get("selected_device")
        if isinstance(saved_device, dict):
            self.selected_device_setting = saved_device

        raw_actions = settings.get("actions", [])
        if isinstance(raw_actions, list):
            self.actions = [
                normalized
                for action in raw_actions
                if (normalized := self.normalize_action(action)) is not None
            ]

        raw_detections = settings.get("detections", [])
        if isinstance(raw_detections, list):
            self.detection_history = [
                normalized
                for detection in raw_detections
                if (
                    normalized := self.normalize_detection(detection)
                ) is not None
            ]

    def save_settings(self):
        """Save application settings without risking a partial JSON file."""

        settings = {
            "selected_device": self.selected_device_setting,
            "actions": self.actions,
            "detections": self.detection_history,
        }

        temporary_file = SETTINGS_FILE.with_suffix(".tmp")

        try:
            with temporary_file.open("w", encoding="utf-8") as settings_file:
                json.dump(settings, settings_file, indent=2)
            temporary_file.replace(SETTINGS_FILE)
        except OSError as error:
            messagebox.showerror(
                "Settings Error",
                f"Could not save settings:\n\n{error}"
            )

    def normalize_action(self, action):
        """Return a safe action dictionary or None for an invalid entry."""

        if not isinstance(action, dict):
            return None

        action_type = action.get("type")
        sequence = str(action.get("sequence", "")).upper().strip()
        target = str(action.get("target", "")).strip()

        if (
            action_type not in ACTION_TYPE_LABELS
            or not sequence
            or not target
            or any(key not in DTMF_MAP.values() for key in sequence)
        ):
            return None

        name = str(action.get("name", "")).strip()
        if not name:
            name = sequence

        return {
            "name": name,
            "sequence": sequence,
            "type": action_type,
            "target": target,
            "arguments": str(action.get("arguments", "")).strip(),
        }

    def normalize_detection(self, detection):
        """Return a safe persisted detection record or None."""

        if not isinstance(detection, dict):
            return None

        timestamp = str(detection.get("timestamp", "")).strip()
        sequence = str(detection.get("sequence", "")).upper().strip()

        try:
            tone_count = int(detection.get("tone_count", len(sequence)))
        except (TypeError, ValueError):
            return None

        if (
            not timestamp
            or not sequence
            or tone_count < 1
            or any(key not in DTMF_MAP.values() for key in sequence)
        ):
            return None

        return {
            "timestamp": timestamp,
            "sequence": sequence,
            "tone_count": tone_count,
        }

    # ========================================================
    # GUI
    # ========================================================

    def build_gui(self):
        main = ttk.Frame(self.root, padding=10)
        main.pack(fill="both", expand=True)

        # ----------------------------------------------------
        # Device frame
        # ----------------------------------------------------

        device_frame = ttk.LabelFrame(
            main,
            text="Audio Input",
            padding=10
        )

        device_frame.pack(fill="x", pady=(0, 10))

        ttk.Label(
            device_frame,
            text="Microphone:"
        ).grid(
            row=0,
            column=0,
            sticky="w",
            padx=(0, 8)
        )

        self.device_combo = ttk.Combobox(
            device_frame,
            state="readonly",
            width=75
        )

        self.device_combo.grid(
            row=0,
            column=1,
            sticky="ew"
        )

        self.device_combo.bind(
            "<<ComboboxSelected>>",
            self.on_device_selected
        )

        ttk.Button(
            device_frame,
            text="Refresh",
            command=self.refresh_devices
        ).grid(
            row=0,
            column=2,
            padx=(8, 0)
        )

        device_frame.columnconfigure(1, weight=1)

        # ----------------------------------------------------
        # Detection settings
        # ----------------------------------------------------

        settings_frame = ttk.LabelFrame(
            main,
            text="Detection",
            padding=10
        )

        settings_frame.pack(fill="x", pady=(0, 10))

        ttk.Label(
            settings_frame,
            text="Minimum tone:"
        ).grid(
            row=0,
            column=0,
            sticky="w"
        )

        self.min_tone_spin = ttk.Spinbox(
            settings_frame,
            from_=20,
            to=500,
            increment=10,
            textvariable=self.min_tone_ms,
            width=8
        )

        self.min_tone_spin.grid(
            row=0,
            column=1,
            padx=(5, 2)
        )

        ttk.Label(
            settings_frame,
            text="ms"
        ).grid(
            row=0,
            column=2,
            sticky="w"
        )

        # ----------------------------------------------------
        # Start/stop
        # ----------------------------------------------------

        controls = ttk.Frame(main)
        controls.pack(fill="x", pady=(0, 10))

        self.start_button = ttk.Button(
            controls,
            text="Start Listening",
            command=self.start_listening
        )

        self.start_button.pack(side="left")

        self.stop_button = ttk.Button(
            controls,
            text="Stop",
            command=self.stop_listening,
            state="disabled"
        )

        self.stop_button.pack(
            side="left",
            padx=(8, 0)
        )

        self.status_label = ttk.Label(
            controls,
            text="Stopped"
        )

        self.status_label.pack(
            side="right"
        )

        # ----------------------------------------------------
        # Live decoded sequence
        # ----------------------------------------------------

        sequence_frame = ttk.LabelFrame(
            main,
            text="Decoded Sequence",
            padding=10
        )

        sequence_frame.pack(
            fill="x",
            pady=(0, 10)
        )

        self.sequence_var = tk.StringVar(value="")

        self.sequence_entry = ttk.Entry(
            sequence_frame,
            textvariable=self.sequence_var,
            font=("Consolas", 18)
        )

        self.sequence_entry.pack(
            side="left",
            fill="x",
            expand=True
        )

        ttk.Button(
            sequence_frame,
            text="Copy",
            command=self.copy_sequence
        ).pack(
            side="left",
            padx=(8, 0)
        )

        # ----------------------------------------------------
        # Current tone information
        # ----------------------------------------------------

        live_frame = ttk.Frame(main)
        live_frame.pack(fill="x", pady=(0, 10))

        ttk.Label(
            live_frame,
            text="Current tone:"
        ).pack(side="left")

        self.current_tone_label = ttk.Label(
            live_frame,
            text="None",
            font=("Consolas", 14, "bold")
        )

        self.current_tone_label.pack(
            side="left",
            padx=(8, 20)
        )

        self.level_label = ttk.Label(
            live_frame,
            text=""
        )

        self.level_label.pack(side="left")

        # ----------------------------------------------------
        # Detection history and configured actions
        # ----------------------------------------------------

        self.notebook = ttk.Notebook(main)
        self.notebook.pack(fill="both", expand=True)

        detections_frame = ttk.Frame(self.notebook, padding=5)
        actions_frame = ttk.Frame(self.notebook, padding=5)

        self.notebook.add(detections_frame, text="Detected Sequences")
        self.notebook.add(actions_frame, text="Actions")

        self.build_detection_list(detections_frame)
        self.build_actions_list(actions_frame)

    def build_detection_list(self, parent):
        """Build the list of DTMF sequences finalized after three seconds."""

        columns = ("number", "timestamp", "sequence", "tone_count")

        list_container = ttk.Frame(parent)
        list_container.pack(fill="both", expand=True)

        self.tree = ttk.Treeview(
            list_container,
            columns=columns,
            show="headings",
            selectmode="browse"
        )

        self.tree.heading("number", text="#")
        self.tree.heading("timestamp", text="Started")
        self.tree.heading("sequence", text="DTMF Sequence")
        self.tree.heading("tone_count", text="Tones")

        self.tree.column("number", width=55, anchor="center", stretch=False)
        self.tree.column("timestamp", width=230, anchor="center", stretch=False)
        self.tree.column("sequence", width=400, anchor="w")
        self.tree.column("tone_count", width=80, anchor="center", stretch=False)

        scrollbar = ttk.Scrollbar(
            list_container,
            orient="vertical",
            command=self.tree.yview
        )

        self.tree.configure(yscrollcommand=scrollbar.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        list_controls = ttk.Frame(parent)
        list_controls.pack(fill="x", pady=(8, 0))

        ttk.Button(
            list_controls,
            text="Clear List",
            command=self.clear_results
        ).pack(side="left")

        ttk.Button(
            list_controls,
            text="Delete Selected",
            command=self.delete_selected_detection
        ).pack(side="left", padx=(8, 0))

        ttk.Button(
            list_controls,
            text="Export CSV...",
            command=self.export_detections_to_csv
        ).pack(side="left", padx=(8, 0))

    def build_actions_list(self, parent):
        """Build the CRUD view for actions triggered by DTMF sequences."""

        action_controls = ttk.Frame(parent)
        action_controls.pack(fill="x", pady=(0, 6))

        ttk.Button(
            action_controls,
            text="New Action",
            command=self.create_action
        ).pack(side="left")

        ttk.Button(
            action_controls,
            text="Edit Selected",
            command=self.edit_selected_action
        ).pack(side="left", padx=(8, 0))

        ttk.Button(
            action_controls,
            text="Delete Selected",
            command=self.delete_selected_action
        ).pack(side="left", padx=(8, 0))

        columns = ("name", "sequence", "action_type", "target")

        self.actions_tree = ttk.Treeview(
            parent,
            columns=columns,
            show="headings",
            selectmode="browse"
        )

        self.actions_tree.heading("name", text="Name")
        self.actions_tree.heading("sequence", text="DTMF Sequence")
        self.actions_tree.heading("action_type", text="Action")
        self.actions_tree.heading("target", text="Program or Sound")

        self.actions_tree.column("name", width=180, anchor="w", stretch=False)
        self.actions_tree.column("sequence", width=150, anchor="center", stretch=False)
        self.actions_tree.column("action_type", width=130, anchor="center", stretch=False)
        self.actions_tree.column("target", width=500, anchor="w")

        scrollbar = ttk.Scrollbar(
            parent,
            orient="vertical",
            command=self.actions_tree.yview
        )

        self.actions_tree.configure(yscrollcommand=scrollbar.set)
        self.actions_tree.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        self.actions_tree.bind("<Double-1>", self.edit_selected_action)

    # ========================================================
    # DETECTION HISTORY
    # ========================================================

    def format_detection_timestamp(self, timestamp):
        """Show saved ISO timestamps in the same readable list format."""

        try:
            return datetime.fromisoformat(timestamp).strftime(
                "%Y-%m-%d %H:%M:%S.%f"
            )[:-3]
        except ValueError:
            return timestamp

    def refresh_detection_tree(self):
        """Render the persisted detection history into the sequence list."""

        for item in self.tree.get_children():
            self.tree.delete(item)

        for index, detection in enumerate(self.detection_history):
            self.tree.insert(
                "",
                "end",
                iid=str(index),
                values=(
                    index + 1,
                    self.format_detection_timestamp(detection["timestamp"]),
                    detection["sequence"],
                    detection["tone_count"],
                )
            )

    def selected_detection_index(self, show_warning=True):
        """Return the selected persisted detection's list index, if any."""

        selection = self.tree.selection()

        if not selection:
            if show_warning:
                messagebox.showwarning(
                    "No Sequence Selected",
                    "Select a sequence in the list first."
                )
            return None

        try:
            index = int(selection[0])
        except ValueError:
            return None

        if 0 <= index < len(self.detection_history):
            return index

        return None

    def delete_selected_detection(self):
        """Delete the selected completed sequence from the history."""

        index = self.selected_detection_index()
        if index is None:
            return

        detection = self.detection_history[index]
        confirmed = messagebox.askyesno(
            "Delete Sequence",
            "Delete the selected DTMF sequence "
            f"'{detection['sequence']}' from the list?"
        )

        if not confirmed:
            return

        del self.detection_history[index]
        self.save_settings()
        self.refresh_detection_tree()

    def export_detections_to_csv(self):
        """Export the full persisted sequence history to a CSV file."""

        if not self.detection_history:
            messagebox.showinfo(
                "No Sequences to Export",
                "There are no completed DTMF sequences to export."
            )
            return

        output_path = filedialog.asksaveasfilename(
            parent=self.root,
            title="Export DTMF Sequences",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")]
        )

        if not output_path:
            return

        try:
            with open(output_path, "w", newline="", encoding="utf-8-sig") as csv_file:
                writer = csv.writer(csv_file)
                writer.writerow(["#", "Started", "DTMF Sequence", "Tones"])

                for index, detection in enumerate(self.detection_history, start=1):
                    writer.writerow(
                        [
                            index,
                            self.format_detection_timestamp(
                                detection["timestamp"]
                            ),
                            detection["sequence"],
                            detection["tone_count"],
                        ]
                    )
        except OSError as error:
            messagebox.showerror(
                "Export Error",
                f"Could not export the sequence list:\n\n{error}"
            )
            return

        self.status_label.configure(
            text=f"Exported {len(self.detection_history)} sequence(s)"
        )

    # ========================================================
    # AUDIO DEVICES
    # ========================================================

    def on_device_selected(self, _event=None):
        """Remember a user-selected microphone by name and host API."""

        selection = self.device_combo.current()

        if selection < 0 or selection >= len(self.input_device_settings):
            return

        self.selected_device_setting = self.input_device_settings[selection]
        self.save_settings()

    def refresh_devices(self):
        try:
            devices = sd.query_devices()

            self.input_devices = []
            self.input_device_settings = []
            display_names = []

            for index, device in enumerate(devices):

                if device["max_input_channels"] > 0:

                    name = (
                        f"{index}: {device['name']} "
                        f"({device['max_input_channels']} ch)"
                    )

                    display_names.append(name)
                    self.input_devices.append(index)
                    self.input_device_settings.append(
                        {
                            "name": str(device["name"]),
                            "hostapi": int(device["hostapi"]),
                        }
                    )

            self.device_combo["values"] = display_names

            saved_position = next(
                (
                    position
                    for position, device_setting in enumerate(
                        self.input_device_settings
                    )
                    if device_setting == self.selected_device_setting
                ),
                None
            )

            # Host API indexes can change after audio drivers are updated.
            # If the saved name is unique, it is still safe to restore it.
            if saved_position is None and self.selected_device_setting:
                name_matches = [
                    position
                    for position, device_setting in enumerate(
                        self.input_device_settings
                    )
                    if (
                        device_setting["name"]
                        == self.selected_device_setting.get("name")
                    )
                ]
                if len(name_matches) == 1:
                    saved_position = name_matches[0]

            if saved_position is not None:
                self.device_combo.current(saved_position)
                return

            # Fall back to the system default when the saved device is not
            # currently attached.  Do not overwrite the saved preference: it
            # may be available again later.
            try:
                default_input = sd.default.device[0]

                if default_input in self.input_devices:
                    self.device_combo.current(
                        self.input_devices.index(default_input)
                    )

                elif display_names:
                    self.device_combo.current(0)

            except Exception:
                if display_names:
                    self.device_combo.current(0)

        except Exception as e:
            messagebox.showerror(
                "Audio Error",
                f"Unable to enumerate audio devices:\n\n{e}"
            )

    # ========================================================
    # ACTIONS
    # ========================================================

    def refresh_actions_tree(self):
        """Refresh the action list after an action is changed."""

        for item in self.actions_tree.get_children():
            self.actions_tree.delete(item)

        for index, action in enumerate(self.actions):
            target = action["target"]
            if action["type"] == ACTION_TYPE_PROGRAM and action["arguments"]:
                target = f"{target} {action['arguments']}"

            self.actions_tree.insert(
                "",
                "end",
                iid=str(index),
                values=(
                    action["name"],
                    action["sequence"],
                    ACTION_TYPE_LABELS[action["type"]],
                    target,
                )
            )

    def selected_action_index(self, show_warning=True):
        """Return the selected action's current list index, if any."""

        selection = self.actions_tree.selection()

        if not selection:
            if show_warning:
                messagebox.showwarning(
                    "No Action Selected",
                    "Select an action first."
                )
            return None

        try:
            index = int(selection[0])
        except ValueError:
            return None

        if 0 <= index < len(self.actions):
            return index

        return None

    def create_action(self):
        self.show_action_editor()

    def edit_selected_action(self, _event=None):
        index = self.selected_action_index(show_warning=_event is None)

        if index is not None:
            self.show_action_editor(index)

    def delete_selected_action(self):
        index = self.selected_action_index()

        if index is None:
            return

        action = self.actions[index]
        confirmed = messagebox.askyesno(
            "Delete Action",
            f"Delete the action '{action['name']}'?"
        )

        if not confirmed:
            return

        del self.actions[index]
        self.save_settings()
        self.refresh_actions_tree()

    def show_action_editor(self, action_index=None):
        """Show a dialog for creating or editing a configured action."""

        action = (
            self.actions[action_index]
            if action_index is not None
            else {
                "name": "",
                "sequence": "",
                "type": ACTION_TYPE_PROGRAM,
                "target": "",
                "arguments": "",
            }
        )

        dialog = tk.Toplevel(self.root)
        dialog.title(
            "Edit Action" if action_index is not None else "New Action"
        )
        dialog.transient(self.root)
        dialog.resizable(True, False)

        content = ttk.Frame(dialog, padding=12)
        content.pack(fill="both", expand=True)
        content.columnconfigure(1, weight=1)

        name_var = tk.StringVar(value=action["name"])
        sequence_var = tk.StringVar(value=action["sequence"])
        action_type_var = tk.StringVar(
            value=ACTION_TYPE_LABELS[action["type"]]
        )
        target_var = tk.StringVar(value=action["target"])
        arguments_var = tk.StringVar(value=action["arguments"])

        ttk.Label(content, text="Name:").grid(
            row=0, column=0, sticky="w", pady=(0, 8)
        )
        name_entry = ttk.Entry(content, textvariable=name_var, width=55)
        name_entry.grid(row=0, column=1, columnspan=2, sticky="ew", pady=(0, 8))

        ttk.Label(content, text="DTMF sequence:").grid(
            row=1, column=0, sticky="w", pady=(0, 8)
        )
        sequence_entry = ttk.Entry(
            content,
            textvariable=sequence_var,
            font=("Consolas", 11),
            width=30
        )
        sequence_entry.grid(row=1, column=1, columnspan=2, sticky="ew", pady=(0, 8))

        ttk.Label(content, text="Action:").grid(
            row=2, column=0, sticky="w", pady=(0, 8)
        )
        action_type_combo = ttk.Combobox(
            content,
            state="readonly",
            values=list(ACTION_TYPE_LABELS.values()),
            textvariable=action_type_var,
            width=25
        )
        action_type_combo.grid(row=2, column=1, columnspan=2, sticky="w", pady=(0, 8))

        target_label = ttk.Label(content)
        target_label.grid(row=3, column=0, sticky="w", pady=(0, 8))
        target_entry = ttk.Entry(content, textvariable=target_var, width=55)
        target_entry.grid(row=3, column=1, sticky="ew", pady=(0, 8))

        def selected_action_type():
            return next(
                (
                    action_type
                    for action_type, label in ACTION_TYPE_LABELS.items()
                    if label == action_type_var.get()
                ),
                None
            )

        def browse_target():
            if selected_action_type() == ACTION_TYPE_PROGRAM:
                selected_path = filedialog.askopenfilename(
                    parent=dialog,
                    title="Select Program",
                    filetypes=[
                        ("Programs", "*.exe *.bat *.cmd *.com"),
                        ("All files", "*.*"),
                    ]
                )
            else:
                selected_path = filedialog.askopenfilename(
                    parent=dialog,
                    title="Select Sound",
                    filetypes=[
                        ("Sound files", "*.wav *.mp3 *.ogg *.flac *.m4a"),
                        ("All files", "*.*"),
                    ]
                )

            if selected_path:
                target_var.set(selected_path)

        ttk.Button(
            content,
            text="Browse...",
            command=browse_target
        ).grid(row=3, column=2, sticky="e", padx=(8, 0), pady=(0, 8))

        arguments_label = ttk.Label(content, text="Arguments:")
        arguments_label.grid(row=4, column=0, sticky="w", pady=(0, 8))
        arguments_entry = ttk.Entry(
            content,
            textvariable=arguments_var,
            width=55
        )
        arguments_entry.grid(row=4, column=1, columnspan=2, sticky="ew", pady=(0, 8))

        help_label = ttk.Label(
            content,
            text="A sequence is complete after 3 seconds without another tone.",
            foreground="gray"
        )
        help_label.grid(row=5, column=0, columnspan=3, sticky="w", pady=(0, 12))

        def update_action_type(*_args):
            is_program = selected_action_type() == ACTION_TYPE_PROGRAM
            target_label.configure(
                text="Program:" if is_program else "Sound file:"
            )
            arguments_label.configure(
                state="normal" if is_program else "disabled"
            )
            arguments_entry.configure(
                state="normal" if is_program else "disabled"
            )

        action_type_var.trace_add("write", update_action_type)
        update_action_type()

        button_row = ttk.Frame(content)
        button_row.grid(row=6, column=0, columnspan=3, sticky="e")

        def save_action():
            sequence = sequence_var.get().upper().strip()
            action_type = selected_action_type()
            target = target_var.get().strip()
            name = name_var.get().strip() or sequence

            if not sequence:
                messagebox.showwarning(
                    "DTMF Sequence Required",
                    "Enter the DTMF sequence that should trigger this action.",
                    parent=dialog
                )
                return

            invalid_keys = [
                key for key in sequence if key not in DTMF_MAP.values()
            ]
            if invalid_keys:
                messagebox.showwarning(
                    "Invalid DTMF Sequence",
                    "A sequence may only contain 0-9, *, #, and A-D.",
                    parent=dialog
                )
                return

            if action_type not in ACTION_TYPE_LABELS:
                messagebox.showwarning(
                    "Action Required",
                    "Choose whether this action runs a program or plays a sound.",
                    parent=dialog
                )
                return

            if not target:
                messagebox.showwarning(
                    "Target Required",
                    "Choose the program or sound file to run.",
                    parent=dialog
                )
                return

            saved_action = {
                "name": name,
                "sequence": sequence,
                "type": action_type,
                "target": target,
                "arguments": (
                    arguments_var.get().strip()
                    if action_type == ACTION_TYPE_PROGRAM
                    else ""
                ),
            }

            if action_index is None:
                self.actions.append(saved_action)
            else:
                self.actions[action_index] = saved_action

            self.save_settings()
            self.refresh_actions_tree()
            dialog.destroy()

        ttk.Button(button_row, text="Cancel", command=dialog.destroy).pack(
            side="right"
        )
        ttk.Button(button_row, text="Save", command=save_action).pack(
            side="right", padx=(0, 8)
        )

        name_entry.focus_set()
        dialog.grab_set()

    def run_matching_actions(self, sequence):
        """Run every configured action whose sequence exactly matches."""

        matches = [
            action for action in self.actions
            if action["sequence"] == sequence
        ]

        if not matches:
            return

        errors = []

        for action in matches:
            try:
                if action["type"] == ACTION_TYPE_PROGRAM:
                    arguments = shlex.split(
                        action["arguments"],
                        posix=False
                    )
                    subprocess.Popen([action["target"], *arguments])
                else:
                    self.play_sound(action["target"])
            except (OSError, ValueError) as error:
                errors.append(f"{action['name']}: {error}")

        if errors:
            messagebox.showerror(
                "Action Error",
                "Could not run the following action(s):\n\n" + "\n".join(errors)
            )
        else:
            action_names = ", ".join(action["name"] for action in matches)
            self.status_label.configure(text=f"Ran action: {action_names}")

    def play_sound(self, sound_path):
        """Start a sound asynchronously without opening a player window."""

        path = Path(sound_path)
        if not path.is_file():
            raise FileNotFoundError(f"Sound file not found: {path}")

        if sys.platform == "win32":
            try:
                self.play_sound_with_pygame(path)
                return
            except (ImportError, OSError, RuntimeError) as pygame_error:
                # Keep MCI as a fallback for environments that do not have
                # pygame or whose SDL mixer cannot handle this file format.
                try:
                    self.play_sound_with_mci(path)
                    return
                except OSError as mci_error:
                    raise OSError(
                        "Could not start the background sound player. "
                        f"pygame: {pygame_error}; MCI: {mci_error}"
                    ) from mci_error
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])

    def play_sound_with_pygame(self, path):
        """Play a sound with SDL's background mixer when it is available."""

        # pygame imports with a banner unless this is set before importing it.
        os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
        import pygame

        if pygame.mixer.get_init() is None:
            pygame.mixer.init()

        sound = pygame.mixer.Sound(str(path))
        channel = sound.play()

        if channel is None:
            raise OSError("No audio channel was available for playback.")

        self.active_sound_channels.append((sound, channel))
        self.remove_finished_sound_channels()

    def remove_finished_sound_channels(self):
        """Keep sound objects alive until their asynchronous playback ends."""

        self.active_sound_channels = [
            (sound, channel)
            for sound, channel in self.active_sound_channels
            if channel.get_busy()
        ]

        if self.active_sound_channels:
            self.root.after(250, self.remove_finished_sound_channels)

    def mci_command(self, command, response_size=0):
        """Run a Windows Multimedia Control Interface command."""

        # MCI commands such as open, play, and close do not return text.  A
        # one-character return buffer makes some media drivers fail with
        # "output string was too large" rather than playing the file.
        response = (
            ctypes.create_unicode_buffer(response_size)
            if response_size
            else None
        )
        result = ctypes.windll.winmm.mciSendStringW(
            command,
            response,
            response_size,
            0
        )

        if result:
            error_message = ctypes.create_unicode_buffer(256)
            ctypes.windll.winmm.mciGetErrorStringW(
                result,
                error_message,
                len(error_message)
            )
            raise OSError(error_message.value or f"MCI error {result}")

        return response.value if response is not None else ""

    def play_sound_with_mci(self, path):
        """Play a Windows sound file asynchronously and close it when done."""

        alias = f"dtmf_sound_{time.monotonic_ns()}"
        extension = path.suffix.lower()
        device_type = {
            ".wav": "waveaudio",
            ".mp3": "mpegvideo",
        }.get(extension)

        open_command = f'open "{path}" alias {alias}'
        if device_type:
            open_command = (
                f'open "{path}" type {device_type} alias {alias}'
            )

        try:
            self.mci_command(open_command)
            self.mci_command(f"play {alias}")
        except OSError:
            try:
                self.mci_command(f"close {alias}")
            except OSError:
                pass
            raise

        def close_when_finished():
            try:
                state = self.mci_command(
                    f"status {alias} mode",
                    response_size=64
                ).lower()
            except OSError:
                # Some MCI drivers close the device as soon as playback
                # ends.  There is nothing left to clean up in that case.
                return

            if state in {"playing", "paused", "seeking"}:
                # MCI devices are associated with the calling UI thread on
                # some Windows installations.  Poll through Tk rather than
                # a Python worker thread so the alias remains visible.
                self.root.after(250, close_when_finished)
                return

            try:
                self.mci_command(f"close {alias}")
            except OSError:
                pass

        self.root.after(250, close_when_finished)

    # ========================================================
    # AUDIO CALLBACK
    # ========================================================

    def audio_callback(
        self,
        indata,
        frames,
        time_info,
        status
    ):
        if status:
            print(status)

        if not self.running:
            return

        # Use first channel
        audio = np.copy(indata[:, 0])

        try:
            self.audio_queue.put_nowait(audio)
        except queue.Full:
            pass

    # ========================================================
    # START / STOP
    # ========================================================

    def start_listening(self):

        selection = self.device_combo.current()

        if selection < 0:
            messagebox.showwarning(
                "No Microphone",
                "Please select an audio input device."
            )
            return

        device_index = self.input_devices[selection]
        self.selected_device_setting = self.input_device_settings[selection]
        self.save_settings()

        try:
            device_info = sd.query_devices(
                device_index,
                "input"
            )

            # Most devices support 48k. If not, use device default.
            preferred_rate = 48000

            try:
                sd.check_input_settings(
                    device=device_index,
                    channels=1,
                    samplerate=preferred_rate
                )

                self.sample_rate = preferred_rate

            except Exception:
                self.sample_rate = int(
                    device_info["default_samplerate"]
                )

            self.detector = DTMFDetector(
                self.sample_rate
            )

            # Roughly 20ms chunks
            self.block_size = max(
                256,
                int(self.sample_rate * 0.020)
            )

            # Empty old data
            while not self.audio_queue.empty():
                try:
                    self.audio_queue.get_nowait()
                except queue.Empty:
                    break

            self.running = True

            self.current_candidate = None
            self.candidate_start = None

            self.active_tone = None
            self.active_start = None

            self.stream = sd.InputStream(
                device=device_index,
                channels=1,
                samplerate=self.sample_rate,
                blocksize=self.block_size,
                dtype="float32",
                callback=self.audio_callback
            )

            self.stream.start()

            self.worker_thread = threading.Thread(
                target=self.detection_worker,
                daemon=True
            )

            self.worker_thread.start()

            self.start_button.configure(
                state="disabled"
            )

            self.stop_button.configure(
                state="normal"
            )

            self.device_combo.configure(
                state="disabled"
            )

            self.status_label.configure(
                text=f"Listening - {self.sample_rate} Hz"
            )

        except Exception as e:

            self.running = False

            messagebox.showerror(
                "Audio Error",
                f"Could not start audio input:\n\n{e}"
            )

    def stop_listening(self):

        self.running = False

        if self.stream is not None:
            try:
                self.stream.stop()
                self.stream.close()
            except Exception:
                pass

            self.stream = None

        self.start_button.configure(
            state="normal"
        )

        self.stop_button.configure(
            state="disabled"
        )

        self.device_combo.configure(
            state="readonly"
        )

        self.status_label.configure(
            text="Stopped"
        )

        self.current_tone_label.configure(
            text="None"
        )

    # ========================================================
    # DETECTION WORKER
    # ========================================================

    def detection_worker(self):

        # Combine several small audio callbacks for better frequency
        # resolution.
        analysis_ms = 40

        analysis_samples = int(
            self.sample_rate * (analysis_ms / 1000.0)
        )

        buffer = np.array([], dtype=np.float32)

        while self.running:

            try:
                chunk = self.audio_queue.get(
                    timeout=0.2
                )

            except queue.Empty:
                continue

            buffer = np.concatenate(
                (buffer, chunk)
            )

            while len(buffer) >= analysis_samples:

                samples = buffer[:analysis_samples]

                # 50% overlap
                step = analysis_samples // 2
                buffer = buffer[step:]

                key, low, high, confidence = (
                    self.detector.detect(samples)
                )

                now = time.monotonic()

                self.process_detected_tone(
                    key,
                    low,
                    high,
                    confidence,
                    now
                )

    # ========================================================
    # TONE STATE / DEBOUNCE
    # ========================================================

    def process_detected_tone(
        self,
        key,
        low,
        high,
        confidence,
        now
    ):

        minimum_duration = (
            self.min_tone_ms.get() / 1000.0
        )

        # ----------------------------------------------------
        # No valid DTMF tone
        # ----------------------------------------------------

        if key is None:

            if self.active_tone is not None:

                duration = now - self.active_start

                self.event_queue.put(
                    (
                        "tone_end",
                        self.active_tone,
                        duration
                    )
                )

            self.current_candidate = None
            self.candidate_start = None

            self.active_tone = None
            self.active_start = None

            self.event_queue.put(
                ("current", None)
            )

            return

        # ----------------------------------------------------
        # Candidate changed
        # ----------------------------------------------------

        if key != self.current_candidate:

            # If another confirmed tone was active,
            # terminate it.
            if self.active_tone is not None:

                duration = now - self.active_start

                self.event_queue.put(
                    (
                        "tone_end",
                        self.active_tone,
                        duration
                    )
                )

                self.active_tone = None
                self.active_start = None

            self.current_candidate = key
            self.candidate_start = now

            return

        # ----------------------------------------------------
        # Same candidate is continuing
        # ----------------------------------------------------

        candidate_duration = (
            now - self.candidate_start
        )

        self.event_queue.put(
            (
                "current",
                key,
                low,
                high,
                candidate_duration,
                confidence
            )
        )

        # ----------------------------------------------------
        # Confirm tone once it persists long enough
        # ----------------------------------------------------

        if (
            self.active_tone is None
            and
            candidate_duration >= minimum_duration
        ):

            self.active_tone = key
            self.active_start = self.candidate_start

            timestamp = datetime.now()

            self.event_queue.put(
                (
                    "detected",
                    key,
                    timestamp,
                    low,
                    high,
                    candidate_duration,
                    confidence
                )
            )

    # ========================================================
    # GUI EVENT HANDLER
    # ========================================================

    def process_events(self):

        try:

            while True:

                event = self.event_queue.get_nowait()

                event_type = event[0]

                # --------------------------------------------
                # Tone detected
                # --------------------------------------------

                if event_type == "detected":

                    (
                        _,
                        key,
                        timestamp,
                        low,
                        high,
                        duration,
                        confidence
                    ) = event

                    self.add_detection(
                        key,
                        timestamp,
                        low,
                        high,
                        duration,
                        confidence
                    )

                # --------------------------------------------
                # Live display
                # --------------------------------------------

                elif event_type == "current":

                    if event[1] is None:

                        self.current_tone_label.configure(
                            text="None"
                        )

                        self.level_label.configure(
                            text=""
                        )

                    else:

                        (
                            _,
                            key,
                            low,
                            high,
                            duration,
                            confidence
                        ) = event

                        self.current_tone_label.configure(
                            text=key
                        )

                        self.level_label.configure(
                            text=(
                                f"{low} + {high} Hz   "
                                f"{duration * 1000:.0f} ms   "
                                f"Confidence {confidence:.1f}x"
                            )
                        )

                # --------------------------------------------
                # Tone ended
                # --------------------------------------------

                elif event_type == "tone_end":
                    pass

        except queue.Empty:
            pass

        self.root.after(
            25,
            self.process_events
        )

    # ========================================================
    # ADD DETECTION TO GROUP
    # ========================================================

    def add_detection(
        self,
        key,
        timestamp,
        low,
        high,
        duration,
        confidence
    ):

        # Once the previous group has been finalized, the next tone starts a
        # new sequence.  The readout therefore always shows only the latest
        # sequence instead of every tone detected during this app session.
        if self.pending_sequence is None:
            self.decoded_sequence = ""
            self.pending_sequence = {
                "sequence": "",
                "timestamp": timestamp,
                "tone_count": 0,
            }

        self.decoded_sequence += key
        self.sequence_var.set(self.decoded_sequence)

        self.pending_sequence["sequence"] += key
        self.pending_sequence["tone_count"] += 1

        # A new tone restarts the timer.  Only a three-second pause turns the
        # pending tones into one row and allows an action to fire.
        if self.sequence_group_timer is not None:
            self.root.after_cancel(self.sequence_group_timer)

        self.sequence_group_timer = self.root.after(
            SEQUENCE_GROUP_DELAY_MS,
            self.finalize_detection_group
        )

    def finalize_detection_group(self):
        """Add the pending burst as one row and trigger any exact match."""

        self.sequence_group_timer = None

        if self.pending_sequence is None:
            return

        pending_sequence = self.pending_sequence
        self.pending_sequence = None

        self.detection_history.append(
            {
                "timestamp": pending_sequence["timestamp"].isoformat(
                    timespec="milliseconds"
                ),
                "sequence": pending_sequence["sequence"],
                "tone_count": pending_sequence["tone_count"],
            }
        )
        self.save_settings()
        self.refresh_detection_tree()

        items = self.tree.get_children()
        if items:
            self.tree.see(items[-1])

        self.run_matching_actions(pending_sequence["sequence"])

    # ========================================================
    # OTHER GUI ACTIONS
    # ========================================================

    def clear_results(self):

        if (
            not self.detection_history
            and self.pending_sequence is None
            and not self.decoded_sequence
        ):
            return

        confirmed = messagebox.askyesno(
            "Clear Sequence List",
            "Clear every saved DTMF sequence from the list?"
        )

        if not confirmed:
            return

        if self.sequence_group_timer is not None:
            self.root.after_cancel(self.sequence_group_timer)
            self.sequence_group_timer = None

        self.pending_sequence = None
        self.detection_history.clear()
        self.decoded_sequence = ""
        self.sequence_var.set("")
        self.save_settings()
        self.refresh_detection_tree()
        self.status_label.configure(text="Sequence list cleared")

    def copy_sequence(self):

        sequence = self.sequence_var.get()

        self.root.clipboard_clear()
        self.root.clipboard_append(sequence)

        self.status_label.configure(
            text=f"Copied: {sequence}"
        )

    def on_close(self):

        self.stop_listening()
        self.root.destroy()


# ============================================================
# MAIN
# ============================================================

def main():

    root = tk.Tk()

    try:
        # Better DPI behavior on Windows
        root.tk.call(
            "tk",
            "scaling",
            1.2
        )
    except Exception:
        pass

    app = DTMFDecoderGUI(root)

    root.mainloop()


if __name__ == "__main__":
    main()
