from flask import Flask, render_template, jsonify, request
import os
import json
import re
import serial
import time

app = Flask(__name__)

DATA_FILE = os.path.join(os.path.dirname(__file__), "tray_items.json")
SECTION_RE = re.compile(r"^[A-E][1-6]$")
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")

RAW_IMAGE_DIR = os.path.join(STATIC_DIR, "images")
ANNOTATED_IMAGE_DIR = os.path.join(STATIC_DIR, "annotated")
DETECTION_JSON_DIR = os.path.join(STATIC_DIR, "detections")
MODEL_PATH = os.path.join(BASE_DIR, "yolo", "my_model.pt")
DETECTION_SYNC_CONFIDENCE = 0.70
ALL_TRAY_SECTIONS = [f"{row}{col}" for row in "ABCDE" for col in range(1, 7)]

SERIAL_PORT = "/dev/ttyACM0"
BAUD_RATE = 9600
TOTAL_TRAYS = 6
ser = None
yolo_model = None

def ensure_dirs():
    os.makedirs(STATIC_DIR, exist_ok=True)
    os.makedirs(RAW_IMAGE_DIR, exist_ok=True)
    os.makedirs(ANNOTATED_IMAGE_DIR, exist_ok=True)
    os.makedirs(DETECTION_JSON_DIR, exist_ok=True)

def init_yolo():
    global yolo_model
    if yolo_model is None:
        yolo_model = YOLO(MODEL_PATH)

def capture_tray_image(tray_id: int, delay_seconds: float = 3.0) -> str:
    ensure_dirs()
    time.sleep(delay_seconds)
    cam = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cam.isOpened():
        raise RuntimeError("Could not open USB webcam on /dev/video0")
    cam.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cam.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    frame = None
    for _ in range(10):
        ok, temp = cam.read()
        if ok:
            frame = temp
        time.sleep(0.05)
    cam.release()
    if frame is None:
        raise RuntimeError("Failed to capture image from webcam")
    filename = f"tray_{tray_id}.jpg"
    save_path = os.path.join(RAW_IMAGE_DIR, filename)
    ok = cv2.imwrite(save_path, frame)
    if not ok:
        raise RuntimeError(f"Failed to write image to {save_path}")
    return f"images/{filename}"

def run_yolo_on_image(image_rel_path: str, tray_id: int) -> dict:
    ensure_dirs()
    init_yolo()
    image_abs_path = os.path.join(STATIC_DIR, image_rel_path)
    if not os.path.exists(image_abs_path):
        raise RuntimeError(f"Image not found: {image_abs_path}")
    results = yolo_model.predict(
        source=image_abs_path,
        conf=DETECTION_SYNC_CONFIDENCE,
        imgsz=640,
        save=False,
        verbose=False
    )
    if not results:
        raise RuntimeError("YOLO returned no results object")
    result = results[0]
    detections = []
    names = result.names if hasattr(result, "names") else {}

    if result.boxes is not None:
        for i, box in enumerate(result.boxes):
            xyxy = box.xyxy[0].tolist()
            conf = float(box.conf[0].item()) if box.conf is not None else 0.0
            cls_id = int(box.cls[0].item()) if box.cls is not None else -1
            class_name = names.get(cls_id, str(cls_id))

            x1, y1, x2, y2 = xyxy
            center_x = (x1 + x2) / 2.0
            center_y = (y1 + y2) / 2.0

            detections.append({
                "id": i,
                "class_id": cls_id,
                "class_name": class_name,
                "confidence": round(conf, 4),
                "box_xyxy": {
                    "x1": round(x1, 2),
                    "y1": round(y1, 2),
                    "x2": round(x2, 2),
                    "y2": round(y2, 2)
                },
                "center": {
                    "x": round(center_x, 2),
                    "y": round(center_y, 2)
                }
            })
    annotated_filename = f"tray_{tray_id}_annotated.jpg"
    annotated_abs_path = os.path.join(ANNOTATED_IMAGE_DIR, annotated_filename)
    annotated_frame = result.plot()
    ok = cv2.imwrite(annotated_abs_path, annotated_frame)
    if not ok:
        raise RuntimeError(f"Failed to save annotated image to {annotated_abs_path}")
    json_filename = f"tray_{tray_id}_detections.json"
    json_abs_path = os.path.join(DETECTION_JSON_DIR, json_filename)
    payload = {
        "tray_id": tray_id,
        "model_path": MODEL_PATH,
        "source_image": image_rel_path,
        "annotated_image": f"annotated/{annotated_filename}",
        "timestamp": int(time.time()),
        "detection_count": len(detections),
        "detections": detections
    }
    with open(json_abs_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    return payload

def _find_first_unoccupied_section(items: list) -> str:
    used_sections = {
        str(it.get("section", "")).strip().upper()
        for it in (items or [])
        if isinstance(it, dict) and str(it.get("section", "")).strip()
    }
    for section in ALL_TRAY_SECTIONS:
        if section not in used_sections:
            return section
    return ""

def _sync_detected_items_into_tray_database(tray_id: int, detection_payload: dict) -> list:
    data = _load_tray_items()
    tray_key = str(tray_id)
    tray_items = [dict(item) for item in data.get(tray_key, []) if isinstance(item, dict)]

    existing_names = {
        str(item.get("name", "")).strip().casefold()
        for item in tray_items
    }

    added_items = []
    for det in detection_payload.get("detections", []):
        confidence = float(det.get("confidence", 0.0))
        if confidence < DETECTION_SYNC_CONFIDENCE:
            continue

        class_name = str(det.get("class_name", "")).strip()
        if not class_name:
            continue

        if class_name.casefold() in existing_names:
            continue

        section = _find_first_unoccupied_section(tray_items)
        if not section:
            continue

        new_item = {
            "name": class_name,
            "qty": 1,
            "section": section
        }

        tray_items.append(new_item)
        existing_names.add(class_name.casefold())
        added_items.append(new_item)

    if added_items:
        data[tray_key] = tray_items
        _save_tray_items(data)

    return added_items


def init_serial():
    global ser
    try:
        if ser is not None and ser.is_open:
            ser.close()
    except Exception:
        pass

    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2, write_timeout=2)
    time.sleep(3)

def _extract_tray_id(filename: str) -> int:
    """Extract a numeric tray id from an image filename. Fallback to 0 if none."""
    match = re.search(r"(\d+)", filename)
    return int(match.group(1)) if match else 0

def _load_tray_items() -> dict:
    if not os.path.exists(DATA_FILE):
        return {}
    try:
        with open(DATA_FILE, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except Exception:
        return {}
    normalized = {}
    for tray_id, items in (raw or {}).items():
        tray_list = []
        if isinstance(items, list):
            for it in items:
                if isinstance(it, dict):
                    name = str(it.get("name", "")).strip()
                    qty = int(it.get("qty", 0))
                    section = str(it.get("section", "")).strip()
                else:
                    name = str(it).strip()
                    qty = 0
                    section = ""
                if name:
                    tray_list.append({"name": name, "qty": max(qty, 0), "section": section})
        normalized[str(tray_id)] = tray_list
    return normalized

def _save_tray_items(data: dict) -> None:
    with open(DATA_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

@app.route("/")
def index():
    image_dir = os.path.join(app.static_folder, "images")
    image_files = [img for img in os.listdir(image_dir) if img.lower().endswith((".png", ".jpg", ".jpeg", ".gif"))]
    # Sort numerically by any digits in filename, then lexicographically as fallback
    image_files.sort(key=lambda x: (_extract_tray_id(x), x.lower()))
    trays = [
        {
            "id": _extract_tray_id(img) or idx + 1,
            "image": f"images/{img}",
            "filename": img,
        }
        for idx, img in enumerate(image_files)
    ]

    tray_items = _load_tray_items()
    # Ensure all trays exist in mapping
    for t in trays:
        tray_items.setdefault(str(t["id"]), [])
    _save_tray_items(tray_items)

    return render_template("index.html", trays=trays, tray_items=tray_items)

@app.get("/api/trays")
def api_get_trays():
    image_dir = os.path.join(app.static_folder, "images")
    if not os.path.isdir(image_dir):
        return jsonify({"trays": [], "items": {}})
    image_files = [img for img in os.listdir(image_dir) if img.lower().endswith((".png", ".jpg", ".jpeg", ".gif"))]
    image_files.sort(key=lambda x: (_extract_tray_id(x), x.lower()))
    trays = [
        {
            "id": _extract_tray_id(img) or idx + 1,
            "image": f"images/{img}",
            "filename": img,
        }
        for idx, img in enumerate(image_files)
    ]
    return jsonify({"trays": trays, "items": _load_tray_items()})

@app.post("/api/tray/<int:tray_id>/items")
def api_update_tray_items(tray_id: int):
    payload = request.get_json(silent=True) or {}
    items = payload.get("items", [])
    if not isinstance(items, list):
        return jsonify({"error": "items must be a list"}), 400
    normalized = []
    for it in items:
        if isinstance(it, dict):
            name = str(it.get("name", "")).strip()
            qty = int(it.get("qty", 0))
            section = str(it.get("section", "")).strip().upper()
        else:
            name = str(it).strip()
            qty = 0
            section = ""
        if name:
            normalized.append({
                "name": name,
                "qty": max(qty, 0),
                "section": section})
    used = {}
    for it in normalized:
        sec = (it.get("section") or "").strip().upper()
        if not sec:
            it["section"] = ""
            continue
        if not SECTION_RE.match(sec):
            return jsonify({"error": "ERROR! THE ONLY ALLOWED TRAY SECTION ARE A1 TO E6!"}), 400
        if sec in used:
            return jsonify({"error": f'ERROR! Tray section {sec} is occupied by "{used[sec]}".'}), 400
        used[sec] = it["name"]
        it["section"] = sec
    data = _load_tray_items()
    data[str(tray_id)] = normalized
    _save_tray_items(data)
    return jsonify({
        "ok": True,
        "tray_id": tray_id,
        "items": normalized
    })

@app.post("/api/tray/<int:tray_id>/capture")
def capture_tray_photo(tray_id: int):
    try:
        image_rel_path = capture_tray_image(tray_id, delay_seconds=3.0)
        yolo_data = run_yolo_on_image(image_rel_path, tray_id)
        auto_added_items = _sync_detected_items_into_tray_database(tray_id, yolo_data)

        return jsonify({
            "ok": True,
            "stage": "vision",
            "tray_id": tray_id,
            "image": image_rel_path,
            "annotated_image": yolo_data["annotated_image"],
            "detection_json": f"detections/tray_{tray_id}_detections.json",
            "detection_count": yolo_data["detection_count"],
            "detections": yolo_data["detections"],
            "auto_added_items": auto_added_items,
            "ts": yolo_data["timestamp"]
        })

    except Exception as e:
        return jsonify({
            "ok": False,
            "stage": "vision",
            "error": str(e)
        }), 500

@app.route("/tray", methods=["POST"])
def handle_tray():
    global ser
    data = request.get_json(silent=True) or {}
    tray_number = data.get("tray")
    action = str(data.get("action", "")).strip().lower()

    try:
        tray_number = int(tray_number)
    except (TypeError, ValueError):
        return jsonify({
            "ok": False,
            "stage": "serial",
            "error": "Invalid tray number"
        }), 400

    if tray_number < 1:
        return jsonify({
            "ok": False,
            "stage": "serial",
            "error": "Tray number must be >= 1"
        }), 400

    try:
        init_serial()
        
        if tray_number > TOTAL_TRAYS:
            return jsonify({
                "ok": False,
                "stage": "serial",
                "error": f"Tray number must be between 1 and {TOTAL_TRAYS}"
            }), 400

        if action == "store" or action == "retrieve":
            signal_value = tray_number
        elif action == "return":
            signal_value = tray_number + TOTAL_TRAYS
        else:
            return jsonify({
                "ok": False,
                "stage": "serial",
                "error": "Invalid action"
            }), 400

        message = f"{signal_value}\n"
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        ser.write(message.encode("utf-8"))
        ser.flush()
        time.sleep(0.2)

        return jsonify({
            "ok": True,
            "stage": "serial",
            "sent_tray": tray_number,
            "action": action,
            "signal_value": signal_value
        })
    
    except Exception as e:
        try:
            if ser is not None:
                ser.close()
        except Exception:
            pass
        ser = None
        return jsonify({
            "ok": False,
            "stage": "serial",
            "error": str(e)
        }), 500
    
if __name__ == "__main__":
    ensure_dirs()
    app.run(host="0.0.0.0", port=5000, debug=True)
