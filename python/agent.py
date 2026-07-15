"""
CuveGuard - agent Python (GROUPE4)

Boucle en continu :
  - lit waterLevelPct / manualMode / pumpOn du device ESP32 sur ThingsBoard (API REST)
  - applique les seuils 30% / 90% (avec anti-rafale) si le mode n'est pas manuel
  - envoie une RPC setPump a l'ESP32 si necessaire
  - publie sa propre telemetrie (agentDecision, observedLevelPct, pumpCommandSent, autoMode)
    sur son device "Python Agent" via MQTT

Lancement : python agent.py [chemin_config.yaml]
"""

import logging
import sys
import time
from pathlib import Path

import yaml
import paho.mqtt.client as mqtt

from tb_client import ThingsBoardREST, to_bool, to_float

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("agent")

START, STOP, HOLD = "START", "STOP", "HOLD"


def load_config(path: str) -> dict:
    cfg_path = Path(path)
    if not cfg_path.exists():
        log.error("Fichier de config introuvable: %s", cfg_path)
        sys.exit(1)
    with cfg_path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f)


class AgentMqtt:
    """Publie la telemetrie de l'agent sur son propre device ThingsBoard."""

    def __init__(self, host: str, token: str):
        self.client = mqtt.Client()
        self.client.username_pw_set(token)
        self.client.connect(host, 1883, keepalive=60)
        self.client.loop_start()

    def publish_telemetry(self, payload: dict) -> None:
        import json

        self.client.publish("v1/devices/me/telemetry", json.dumps(payload), qos=1)


class Decider:
    """Applique les seuils avec hysteresis (30/90) + anti-rafale (delai mini)."""

    def __init__(self, low_pct: float, high_pct: float, min_interval_sec: float):
        self.low_pct = low_pct
        self.high_pct = high_pct
        self.min_interval_sec = min_interval_sec
        self._last_command_ts = 0.0

    def decide(self, level_pct: float, pump_on: bool) -> tuple[str, bool]:
        """Retourne (decision, pump_command_sent)."""
        now = time.monotonic()
        since_last = now - self._last_command_ts

        if level_pct < self.low_pct and not pump_on:
            if since_last < self.min_interval_sec:
                return HOLD, False
            self._last_command_ts = now
            return START, True

        if level_pct > self.high_pct and pump_on:
            if since_last < self.min_interval_sec:
                return HOLD, False
            self._last_command_ts = now
            return STOP, True

        return HOLD, False


def run(config: dict) -> None:
    tb_cfg = config["thingsboard"]
    ctrl_cfg = config["control"]

    tb = ThingsBoardREST(tb_cfg["host"], tb_cfg["username"], tb_cfg["password"])
    tb.login()

    mqtt_agent = AgentMqtt(tb_cfg["host"], tb_cfg["agent_device_token"])

    decider = Decider(
        low_pct=float(ctrl_cfg["low_threshold_pct"]),
        high_pct=float(ctrl_cfg["high_threshold_pct"]),
        min_interval_sec=float(ctrl_cfg["min_command_interval_sec"]),
    )
    poll_interval = float(ctrl_cfg["poll_interval_sec"])
    esp32_device_id = tb_cfg["esp32_device_id"]

    log.info("Agent CuveGuard demarre (poll=%ss, seuils=%s%%/%s%%)",
              poll_interval, decider.low_pct, decider.high_pct)

    while True:
        try:
            telemetry = tb.get_latest_telemetry(
                esp32_device_id, ["waterLevelPct", "manualMode", "pumpOn"]
            )
            level_pct = to_float(telemetry.get("waterLevelPct"), default=None)
            manual_mode = to_bool(telemetry.get("manualMode", False))
            pump_on = to_bool(telemetry.get("pumpOn", False))

            if level_pct is None:
                log.warning("Pas encore de telemetrie waterLevelPct disponible, attente...")
                time.sleep(poll_interval)
                continue

            auto_mode = not manual_mode
            pump_command_sent = False

            if auto_mode:
                decision, pump_command_sent = decider.decide(level_pct, pump_on)
                if decision == START:
                    tb.send_rpc(esp32_device_id, "setPump", True)
                elif decision == STOP:
                    tb.send_rpc(esp32_device_id, "setPump", False)
            else:
                decision = HOLD  # mode manuel : l'agent ne touche pas a la pompe

            log.info(
                "level=%.1f%% manual=%s pumpOn=%s -> decision=%s (rpc=%s)",
                level_pct, manual_mode, pump_on, decision, pump_command_sent,
            )

            mqtt_agent.publish_telemetry({
                "agentDecision": decision,
                "observedLevelPct": level_pct,
                "pumpCommandSent": pump_command_sent,
                "autoMode": auto_mode,
            })

        except Exception:
            log.exception("Erreur pendant le cycle de decision, on continue")

        time.sleep(poll_interval)


if __name__ == "__main__":
    config_path = sys.argv[1] if len(sys.argv) > 1 else "config.yaml"
    cfg = load_config(config_path)
    try:
        run(cfg)
    except KeyboardInterrupt:
        log.info("Arret demande par l'utilisateur")
