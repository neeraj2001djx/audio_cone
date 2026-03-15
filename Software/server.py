from flask import Flask, render_template, request, jsonify, send_from_directory, abort
from waitress import serve
import paho.mqtt.client as mqtt
import threading
import os
import subprocess
import uuid

app = Flask(__name__)

# ================= MQTT CONFIG =================
MQTT_BROKER = "localhost"
MQTT_PORT = 1883

online_devices = {}
device_states = {}
device_volumes = {}

UPLOAD_FOLDER = "static/uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# path to local ffmpeg
FFMPEG_PATH = "./ffmpeg/ffmpeg.exe"


# ================= MQTT =================

def on_connect(client, userdata, flags, rc):
    print("MQTT Connected")

    client.subscribe("devices/+/status")
    client.subscribe("devices/+/state")
    client.subscribe("devices/+/volume")


def on_disconnect(client, userdata, rc):
    print("MQTT Disconnected")


def on_message(client, userdata, msg):

    topic = msg.topic
    payload = msg.payload.decode()

    parts = topic.split("/")

    if len(parts) < 3:
        return

    device_id = parts[1]
    category = parts[2]

    # -------- DEVICE ONLINE/OFFLINE --------
    if category == "status":

        if payload == "online":
            online_devices[device_id] = True
            device_states[device_id] = "idle"

        elif payload == "offline":
            online_devices.pop(device_id, None)
            device_states.pop(device_id, None)
            device_volumes.pop(device_id, None)

    # -------- DEVICE STATE --------
    elif category == "state":
        device_states[device_id] = payload

    # -------- DEVICE VOLUME --------
    elif category == "volume":
        try:
            device_volumes[device_id] = float(payload)
        except:
            pass


mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message

mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)

threading.Thread(
    target=mqtt_client.loop_forever,
    daemon=True
).start()


# ================= ROUTES =================

@app.route("/")
def dashboard():
    return render_template("dashboard.html")


@app.route("/devices")
def devices():

    return jsonify({
        "devices": list(online_devices.keys()),
        "states": device_states,
        "volumes": device_volumes
    })


# ================= AUDIO CONVERSION THREAD =================

def convert_and_publish(device_id, temp_path):

    temp_id = str(uuid.uuid4())

    # change extension to WAV
    final_name = temp_id + ".wav"
    final_path = os.path.join(UPLOAD_FOLDER, final_name)

    print("Converting audio with ffmpeg")

    try:

        subprocess.run([
            FFMPEG_PATH,
            "-y",
            "-i", temp_path,

            "-ac", "1",          # mono
            "-ar", "44100",      # sample rate
            "-sample_fmt", "s16",# 16 bit PCM
            "-acodec", "pcm_s16le",

            final_path
        ], check=True)

    except subprocess.CalledProcessError:

        print("FFmpeg conversion failed")

        try:
            os.remove(temp_path)
        except:
            pass

        mqtt_client.publish(f"devices/{device_id}/state", "conversion_failed")
        return

    try:
        os.remove(temp_path)
    except:
        pass

    print("Audio ready:", final_name)

    mqtt_client.publish(
        f"devices/{device_id}/command",
        final_name
    )


# ================= AUDIO UPLOAD =================

@app.route("/upload/<device_id>", methods=["POST"])
def upload(device_id):

    if "file" not in request.files:
        return "No file", 400

    file = request.files["file"]

    if file.filename == "":
        return "Empty filename", 400

    print("Upload started for device:", device_id)

    # Immediately notify dashboard
    mqtt_client.publish(
        f"devices/{device_id}/state",
        "download_pending"
    )

    # temporary filename
    temp_id = str(uuid.uuid4())
    temp_path = os.path.join(UPLOAD_FOLDER, temp_id)

    file.save(temp_path)

    # conversion in background thread
    threading.Thread(
        target=convert_and_publish,
        args=(device_id, temp_path),
        daemon=True
    ).start()

    return "OK"


# ================= VOLUME CONTROL =================

@app.route("/volume/<device_id>", methods=["POST"])
def set_volume(device_id):

    data = request.json

    volume = float(data.get("volume", 1.0))

    mqtt_client.publish(
        f"devices/{device_id}/volume",
        volume,
	retain=True
    )

    device_volumes[device_id] = volume

    return "OK"


# ================= AUDIO SERVE =================

@app.route("/audio/<filename>")
def serve_audio(filename):

    safe_path = os.path.join(UPLOAD_FOLDER, filename)

    if not os.path.exists(safe_path):
        abort(404)

    return send_from_directory(
        UPLOAD_FOLDER,
        filename,
        as_attachment=False,
	conditional=True
    )


# ================= SERVER =================

if __name__ == "__main__":

    print("Starting server...")

    serve(
        app,
        host="0.0.0.0",
        port=5000
    )
