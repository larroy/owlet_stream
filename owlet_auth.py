#!/usr/bin/env python3
"""
Owlet Dream camera authenticator.
1. Firebase email/password → ID token
2. GET camera-kms.owletdata.com/kms/{dsn} → {tutkid, password, authKey}
"""
import requests
import json
import sys

FIREBASE_API_KEY = "AIzaSyDlfp3urNTbyhCtHOCxZBOjHQf4WuN_Aws"
FIREBASE_SIGNIN_URL = (
    "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword"
    f"?key={FIREBASE_API_KEY}"
)
KMS_BASE_URL = "https://camera-kms.owletdata.com/kms/"


def firebase_login(email: str, password: str) -> str:
    resp = requests.post(
        FIREBASE_SIGNIN_URL,
        json={"email": email, "password": password, "returnSecureToken": True},
        timeout=15,
    )
    resp.raise_for_status()
    return resp.json()["idToken"]


def get_tutk_credentials(dsn: str, firebase_token: str) -> dict:
    resp = requests.get(
        KMS_BASE_URL + dsn,
        headers={"Authorization": firebase_token},
        timeout=15,
    )
    resp.raise_for_status()
    return resp.json()


def list_cameras(firebase_token: str) -> list:
    """Try to list registered cameras via Owlet device API."""
    resp = requests.get(
        "https://ayla-sso.owletdata.com/api/v1/auth_provider_tokens",
        headers={"Authorization": f"Bearer {firebase_token}"},
        timeout=15,
    )
    if not resp.ok:
        return []
    return resp.json()


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <email> <password> [camera_dsn]", file=sys.stderr)
        print("  If camera_dsn is omitted, tries to discover cameras.", file=sys.stderr)
        sys.exit(1)

    email, password = sys.argv[1], sys.argv[2]
    dsn = sys.argv[3] if len(sys.argv) > 3 else None

    print("Authenticating with Firebase...", file=sys.stderr)
    token = firebase_login(email, password)
    print(f"Got Firebase token: {token[:40]}...", file=sys.stderr)

    if dsn is None:
        print("No DSN provided; attempting camera discovery...", file=sys.stderr)
        cameras = list_cameras(token)
        if cameras:
            print(f"Cameras: {json.dumps(cameras, indent=2)}", file=sys.stderr)
        else:
            print("Could not discover cameras automatically. Provide DSN manually.", file=sys.stderr)
            print(json.dumps({"firebase_token": token}))
            sys.exit(0)

    print(f"Fetching TUTK credentials for DSN={dsn}...", file=sys.stderr)
    creds = get_tutk_credentials(dsn, token)

    # Output all credentials needed by owlet_stream
    result = {
        "dsn": dsn,
        "tutkid": creds.get("tutkid"),
        "password": creds.get("password"),
        "authKey": creds.get("authKey"),
        "firebase_token": token,
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
