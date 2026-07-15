"""Petit client REST ThingsBoard : login, lecture de telemetrie, envoi de RPC."""

import logging

import requests

log = logging.getLogger("tb_client")


class ThingsBoardREST:
    def __init__(self, host: str, username: str, password: str, timeout: float = 10.0):
        self.base_url = f"https://{host}"
        self.username = username
        self.password = password
        self.timeout = timeout
        self._token = None

    def login(self) -> None:
        resp = requests.post(
            f"{self.base_url}/api/auth/login",
            json={"username": self.username, "password": self.password},
            timeout=self.timeout,
        )
        resp.raise_for_status()
        self._token = resp.json()["token"]
        log.info("Connexion ThingsBoard OK (%s)", self.username)

    def _headers(self) -> dict:
        if self._token is None:
            self.login()
        return {"X-Authorization": f"Bearer {self._token}"}

    def _request_with_retry(self, method: str, url: str, **kwargs) -> requests.Response:
        resp = requests.request(method, url, headers=self._headers(), timeout=self.timeout, **kwargs)
        if resp.status_code == 401:
            # token expire -> on se reconnecte une fois
            self.login()
            resp = requests.request(method, url, headers=self._headers(), timeout=self.timeout, **kwargs)
        resp.raise_for_status()
        return resp

    def get_latest_telemetry(self, device_id: str, keys: list[str]) -> dict:
        url = f"{self.base_url}/api/plugins/telemetry/DEVICE/{device_id}/values/timeseries"
        resp = self._request_with_retry("GET", url, params={"keys": ",".join(keys)})
        raw = resp.json()
        result = {}
        for key, points in raw.items():
            if points:
                result[key] = points[0]["value"]
        return result

    def send_rpc(self, device_id: str, method: str, value) -> None:
        url = f"{self.base_url}/api/rpc/oneway/{device_id}"
        payload = {"method": method, "params": {"value": value}}
        self._request_with_retry("POST", url, json=payload)
        log.info("RPC envoyee au device %s: %s(%s)", device_id, method, value)


def to_bool(value) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() == "true"
    return bool(value)


def to_float(value, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default
