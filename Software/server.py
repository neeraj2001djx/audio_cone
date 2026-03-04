from flask import Flask, render_template, request, jsonify, send_from_directory
from waitress import serve
import paho.mqtt.client as mqtt
import threading
import os

app = Flask(__name__)

# ================= MQTT CONFIG =================
MQTT_BROKER = "localhost"
MQTT_PORT = 1883

online_devices = {}
device_states = {}
device_volumes = {}

UPLOAD_FOLDER = "static/uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# ================= MQTT =================

def on_connect(client, userdata, flags, rc):
    print("MQTT Connected")
    client.subscribe("devices/+/status")
    client.subscribe("devices/+/state")
    client.subscribe("devices/+/volume")

def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode()
    parts = topic.split("/")

    if len(parts) < 3:
        return

    device_id = parts[1]
    category = parts[2]

    if category == "status":
        if payload == "online":
            online_devices[device_id] = True
            device_states[device_id] = "idle"
        elif payload == "offline":
            online_devices.pop(device_id, None)
            device_states.pop(device_id, None)
            device_volumes.pop(device_id, None)

    elif category == "state":
        device_states[device_id] = payload

    elif category == "volume":
        try:
            device_volumes[device_id] = float(payload)
        except:
            pass


mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)

threading.Thread(target=mqtt_client.loop_forever, daemon=True).start()

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

@app.route("/upload/<device_id>", methods=["POST"])
def upload(device_id):
    file = request.files["file"]
    filepath = os.path.join(UPLOAD_FOLDER, file.filename)
    file.save(filepath)

    mqtt_client.publish(f"devices/{device_id}/command", file.filename)
    return "OK"

@app.route("/volume/<device_id>", methods=["POST"])
def set_volume(device_id):
    data = request.json
    volume = float(data.get("volume", 1.0))

    mqtt_client.publish(f"devices/{device_id}/volume", volume)
    device_volumes[device_id] = volume
    return "OK"

@app.route("/audio/<filename>")
def serve_audio(filename):
    return send_from_directory(UPLOAD_FOLDER, filename)

if __name__ == "__main__":
    serve(app, host="0.0.0.0", port=5000)