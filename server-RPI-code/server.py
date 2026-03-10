import RPi.GPIO as GPIO
from mfrc522 import SimpleMFRC522
from flask import Flask, jsonify, request, render_template
import threading
import time
import json
from datetime import datetime

# --- Configuration ---
# Uses the /static/ folder for images
AUTHORIZED_USERS = {
    "782066254185": {
        "name": "Kunal",
        "role": "Geologist",
        "profile_image": "/static/profile.png"
    },
    "1041750295380": {
        "name": "Pushkar",
        "role": "Supervisor",
        "profile_image": "/static/profile.png"
    },
    "789372535611": {
        "name": "Pranaav",
        "role": "Miner",
        "profile_image": "/static/profile.png"
    },
    "120681202479": {
        "name": "Gaurav",
        "role": "Technician",
        "profile_image": "/static/profile.png"
    }
}
LOG_FILE = 'log.json'
SENSOR_LOG_FILE = 'sensorslogs.json'

# --- Alert Thresholds ---
TEMP_THRESHOLD = 30
GAS_THRESHOLD = 800
PULSE_THRESHOLD = 100 # Assuming 100 BPM is the high pulse threshold

# --- Global State Variables ---
active_sessions = {}
active_alerts = [] # List to hold pop-up messages
session_lock = threading.Lock()
alert_lock = threading.Lock()

app = Flask(__name__)

# --- API Endpoints ---
@app.route('/status', methods=['GET'])
def get_status():
    """Endpoint reporting active sessions with the JSON format you requested."""
    global active_sessions
    with session_lock:
        if active_sessions:
            active_user_list = [{"tag_id": tag_id} for tag_id in active_sessions.keys()]
            response = {
                "active_users_count": len(active_sessions),
                "users": active_user_list
            }
        else:
            response = {
                "active_users_count": 0, 
                "users": []
            }
    return jsonify(response)

@app.route('/sensordata', methods=['POST'])
def sensor_data():
    """Receives sensor data, checks thresholds, and logs it."""
    if not request.is_json:
        return jsonify({"error": "Request must be JSON"}), 400
    
    data = request.get_json()
    data['timestamp'] = datetime.now().isoformat()
    log_sensor_data_to_file(data)
    
    # --- Threshold Check ---
    try:
        user_id = str(data.get('id'))
        user_name = AUTHORIZED_USERS.get(user_id, {}).get("name", f"Unknown ID {user_id}")
        
        alerts_to_add = []
        
        # Check each value
        temp = data.get('temparutre')
        if temp and temp > TEMP_THRESHOLD:
            alerts_to_add.append(f" HIGH TEMP ALERT 🔥\nWorker: {user_name} (ID: {user_id})\nTemperature: {temp}°C")
            
        gas = data.get('gas senoser') # Using your key "gas senoser"
        if gas and gas > GAS_THRESHOLD:
            alerts_to_add.append(f" HIGH GAS ALERT 💨\nWorker: {user_name} (ID: {user_id})\nGas Level: {gas}")
            
        pulse = data.get('pluse') # Using your key "pluse"
        if pulse and pulse > PULSE_THRESHOLD:
            alerts_to_add.append(f" HIGH PULSE ALERT ❤️\nWorker: {user_name} (ID: {user_id})\nPulse: {pulse} BPM")

        if alerts_to_add:
            with alert_lock:
                active_alerts.extend(alerts_to_add)
                
    except Exception as e:
        print(f"Error processing sensor data: {e}")

    print("--- Received Sensor Data ---")
    print(json.dumps(data, indent=2))
    print("------------------------------")
    
    return jsonify({"status": "success", "message": "Sensor data logged"}), 200

@app.route('/get_alerts', methods=['GET'])
def get_alerts():
    """Returns the current list of alerts to the browser and clears it."""
    global active_alerts
    with alert_lock:
        alerts_to_send = list(active_alerts) # Copy the list
        active_alerts.clear() # Clear the original list
    return jsonify(alerts_to_send)

# --- Admin Dashboard Web Pages ---
@app.route('/admin')
def admin_dashboard():
    return render_template('admin.html', users=AUTHORIZED_USERS)

@app.route('/details/<tag_id>')
def user_details(tag_id):
    user_info = AUTHORIZED_USERS.get(tag_id)
    if not user_info:
        return "User not found", 404
    
    user_info['tag_id'] = tag_id
    
    try:
        with open(LOG_FILE, 'r') as f: all_logs = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError): all_logs = []
    user_logs = [log for log in all_logs if log['tag_id'] == tag_id]
    
    return render_template('details.html', user=user_info, logs=user_logs)

# --- Helper Functions ---
def log_session_to_file(session_data):
    """Appends session data to the session log file."""
    try:
        with open(LOG_FILE, 'r') as f: logs = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError): logs = []
    logs.append(session_data)
    with open(LOG_FILE, 'w') as f: json.dump(logs, f, indent=4)
    print(f" Session logged to {LOG_FILE}")

def log_sensor_data_to_file(sensor_data):
    """Appends sensor data to the sensor log file."""
    try:
        with open(SENSOR_LOG_FILE, 'r') as f: logs = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError): logs = []
    logs.append(sensor_data)
    with open(SENSOR_LOG_FILE, 'w') as f: json.dump(logs, f, indent=4)

# --- RFID Scanner Logic ---
def rfid_scanner_thread():
    global active_sessions
    reader = SimpleMFRC522()
    print(" RFID Scanner thread started. Waiting for tags...")
    while True:
        try:
            scanned_id, text = reader.read()
            str_scanned_id = str(scanned_id)
            print(f"--- Tag Scanned: {str_scanned_id} ---")

            if str_scanned_id in AUTHORIZED_USERS:
                user_name = AUTHORIZED_USERS[str_scanned_id]["name"]
                with session_lock:
                    if str_scanned_id in active_sessions:
                        start_time = active_sessions.pop(str_scanned_id)
                        end_time = datetime.now()
                        duration = end_time - start_time
                        log_entry = {
                            "date": start_time.date().isoformat(),
                            "tag_id": str_scanned_id,
                            "name": user_name,
                            "start_time": start_time.isoformat(),
                            "end_time": end_time.isoformat(),
                            "duration_seconds": round(duration.total_seconds(), 2)
                        }
                        log_session_to_file(log_entry)
                        print(f" Session ended for: {user_name}.")
                    else:
                        active_sessions[str_scanned_id] = datetime.now()
                        print(f" Session started for: {user_name}")
            else:
                print("Unauthorized tag.")
            
            time.sleep(3) # 3-second delay after a scan
        except Exception as e:
            print(f"Error in RFID thread: {e}")
            time.sleep(2)

# --- Main Execution ---
if __name__ == '__main__':
    try:
        scanner_thread = threading.Thread(target=rfid_scanner_thread, daemon=True)
        scanner_thread.start()
        print(" Starting Flask server...")
        print(f"   Admin Dashboard available at http://<your-pi-ip>:5000/admin")
        app.run(host='0.0.0.0', port=5000)
    except KeyboardInterrupt:
        print("\nProgram terminated.")
    finally:
        GPIO.cleanup()