#!/usr/bin/env python3
import os
import json
import csv
import re
import requests
from datetime import datetime, timedelta

DATA_DIR = "/data"
CACHE_FILE_PATH = os.path.join(DATA_DIR, "workout_history_cache.json")
JSON_OUTPUT_PATH = "/www/rowing_stats.json"
API_KEY = os.getenv("CONCEPT2_API_KEY")

# Global RAM cache to hold the best metrics during the loop execution pass
RAM_PR_INDEX = {}

def parse_time_to_seconds(time_str):
    if not time_str or time_str in ["0", "N/A"]:
        return float('inf')
    parts = str(time_str).split(':')
    if len(parts) == 3:
        return int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
    elif len(parts) == 2:
        return int(parts[0]) * 60 + float(parts[1])
    return float(time_str)

def format_seconds_to_string(total_seconds):
    if total_seconds == float('inf') or total_seconds == 0:
        return "0"
    minutes = int(total_seconds // 60)
    seconds = total_seconds % 60
    return f"{minutes}:{seconds:04.1f}"

def load_cached_workouts():
    if os.path.exists(CACHE_FILE_PATH):
        try:
            with open(CACHE_FILE_PATH, 'r') as f:
                return json.load(f)
        except Exception:
            print("[WARN] Local workout cache format unreadable. Forcing clean state rebuild.")
    return {}

def save_cached_workouts(cache_dict):
    with open(CACHE_FILE_PATH, 'w') as f:
        json.dump(cache_dict, f, indent=2)

def fetch_incremental_results(cache_dict):
    new_results = []
    url = "https://log.concept2.com/api/users/me/results"
    headers = {
        "Authorization": f"Bearer {API_KEY}",
        "Accept": "application/vnd.c2logbook.v1+json"
    }
    params = {"limit": 50, "page": 1}
    
    pages_processed = 0
    hit_existing_cache = False
    
    while url and not hit_existing_cache:
        response = requests.get(url, headers=headers, params=params)
        response.raise_for_status()
        payload = response.json()
        
        data_chunk = payload.get("data", [])
        if not data_chunk:
            break
            
        pages_processed += 1
        
        for workout in data_chunk:
            workout_id = str(workout.get("id"))
            if workout_id in cache_dict:
                hit_existing_cache = True
                break
            new_results.append(workout)
            
        pagination = payload.get("meta", {}).get("pagination", {})
        current_page = pagination.get("current_page", 1)
        total_pages = pagination.get("total_pages", 1)
        
        if current_page < total_pages and not hit_existing_cache:
            params["page"] = current_page + 1
        else:
            url = None
            
    print(f"└── [NETWORK] Checked {pages_processed} page(s). Added {len(new_results)} brand new pieces.")
    return new_results

def initialize_ram_pr_index():
    global RAM_PR_INDEX
    distance_types = ["1k", "2k", "5k", "10k"]
    time_types = ["30min", "1hr"]
    
    for w_type in distance_types:
        csv_file = os.path.join(DATA_DIR, f"{w_type}_PR.csv")
        best_time_str = "0"
        best_seconds = float('inf')
        
        if os.path.exists(csv_file):
            with open(csv_file, 'r') as f:
                reader = csv.reader(f)
                next(reader, None)
                for row in reader:
                    if row:
                        row_seconds = parse_time_to_seconds(row[1])
                        if row_seconds < best_seconds:
                            best_seconds = row_seconds
                            best_time_str = row[1]
        RAM_PR_INDEX[w_type] = {"time_str": best_time_str, "seconds": best_seconds}
        
    for w_type in time_types:
        csv_file = os.path.join(DATA_DIR, f"{w_type}_PR.csv")
        best_meters = 0
        
        if os.path.exists(csv_file):
            with open(csv_file, 'r') as f:
                reader = csv.reader(f)
                next(reader, None)
                for row in reader:
                    if row and int(row[1]) > best_meters:
                        best_meters = int(row[1])
        RAM_PR_INDEX[w_type] = {"meters": best_meters}

def parse_pr_from_comment(comment, target_type):
    if not comment:
        return None
    comment_clean = " ".join(comment.lower().split())
    type_patterns = {
        "1k": r"\b1k\b",
        "2k": r"\b2k\b",
        "5k": r"\b5k\b",
        "10k": r"\b10k\b",
        "30min": r"\b(30\s*mins?|30\s*minutes)\b",
        "1hr": r"\b(1\s*hr|1\s*hour)\b"
    }
    pattern = type_patterns.get(target_type)
    if not pattern or not re.search(pattern, comment_clean):
        return None

    if target_type in ["30min", "1hr"]:
        # Look for a 4-5 digit number followed by 'm' or 'meters'
        distance_match = re.search(r"\b(\d{4,5})\s*m(?:eters)?\b", comment_clean)
        if distance_match:
            return distance_match.group(1)
        # Fallback to a raw 4-5 digit number near the match if 'm' was left off
        fallback_match = re.search(r"\b(\d{4,5})\b", comment_clean)
        if fallback_match:
            return fallback_match.group(1)
    else:
        # Look for a standard clock time string (e.g., 3:14.2)
        time_match = re.search(r"\b(\d{1,2}:\d{2}(?::\d{2})?(?:\.\d)?)\b", comment_clean)
        if time_match:
            return time_match.group(1)
    return None

def process_result_row_in_ram(workout_type, current_workout_time, workout_date_str, is_live_announcement):
    global RAM_PR_INDEX
    csv_file = os.path.join(DATA_DIR, f"{workout_type}_PR.csv")
    
    best_seconds = RAM_PR_INDEX[workout_type]["seconds"]
    incoming_seconds = parse_time_to_seconds(current_workout_time)
    
    if incoming_seconds < best_seconds:
        if is_live_announcement:
            print(f"    └── [NEW RECORD] Smashing PR hit for {workout_type}: {current_workout_time}!")
            
        RAM_PR_INDEX[workout_type]["seconds"] = incoming_seconds
        RAM_PR_INDEX[workout_type]["time_str"] = current_workout_time
        
        file_exists = os.path.exists(csv_file)
        with open(csv_file, 'a', newline='') as f:
            writer = csv.writer(f)
            if not file_exists:
                writer.writerow(["Date", "Time"])
            writer.writerow([workout_date_str, current_workout_time])
            
    return RAM_PR_INDEX[workout_type]["time_str"]

def process_time_pr_in_ram(workout_type, current_meters, workout_date_str, is_live_announcement):
    global RAM_PR_INDEX
    csv_file = os.path.join(DATA_DIR, f"{workout_type}_PR.csv")
    
    best_meters = RAM_PR_INDEX[workout_type]["meters"]
    incoming_meters = int(current_meters) if current_meters else 0
    
    if incoming_meters > best_meters:
        if is_live_announcement and incoming_meters > 0:
            print(f"    └── [NEW RECORD] More distance crushed in {workout_type}: {incoming_meters}m!")
            
        RAM_PR_INDEX[workout_type]["meters"] = incoming_meters
        
        file_exists = os.path.exists(csv_file)
        with open(csv_file, 'a', newline='') as f:
            writer = csv.writer(f)
            if not file_exists:
                writer.writerow(["Date", "Meters"])
            writer.writerow([workout_date_str, incoming_meters])
            
    return f"{RAM_PR_INDEX[workout_type]['meters']:,}m"

if __name__ == "__main__":
    print(f"[{datetime.now()}] Triggering Chronological Sequence Cycle...")
    if not API_KEY:
        print("[ERROR] CONCEPT2_API_KEY environment variable is not defined.")
        exit(1)
        
    try:
        workout_cache = load_cached_workouts()
        new_items = fetch_incremental_results(workout_cache)
        
        for w in new_items:
            w_id = str(w.get("id"))
            workout_cache[w_id] = w
            
        if not workout_cache:
            raise ValueError("No historical data found locally and API returned nothing.")
            
        save_cached_workouts(workout_cache)
        initialize_ram_pr_index()
        
        all_chronological_workouts = sorted(workout_cache.values(), key=lambda x: x.get("date", ""), reverse=False)
        
        raw_1k, raw_2k, raw_5k, raw_10k = "0", "0", "0", "0"
        raw_30m, raw_1hr = 0, 0
        marathons_count = 0
        
        lifetime_meters = 0
        season_meters = 0
        rowed_dates_set = set()
        
        current_year = datetime.now().year
        if datetime.now().month >= 5:
            season_start = datetime(current_year, 5, 1)
        else:
            season_start = datetime(current_year - 1, 5, 1)

        total_elements = len(all_chronological_workouts)
        
        for index, workout in enumerate(all_chronological_workouts):
            if workout.get("type") != "rower":
                continue
                
            is_live_announcement = (index == total_elements - 1)
            
            distance = int(workout.get("distance", 0))
            rest_distance = int(workout.get("rest_distance", 0))
            total_workout_distance = distance + rest_distance
            
            raw_time_tenths = int(workout.get("time", 0))
            time_seconds = float(raw_time_tenths) * 0.1
            workout_date_str = workout.get("date", "").split(" ")[0]
            comment = workout.get("comments")
            
            # Process metrics for ALL workout types first (including intervals)
            lifetime_meters += total_workout_distance
            if workout_date_str:
                rowed_dates_set.add(workout_date_str)
                w_date = datetime.strptime(workout_date_str, "%Y-%m-%d")
                if w_date >= season_start:
                    season_meters += total_workout_distance
            
            # Eject interval sessions after accounting for their mileage and streak dates
            w_type_str = str(workout.get("workout_type", "")).lower()
            if "interval" in w_type_str:
                continue
                
            w_1k, w_2k, w_5k, w_10k = "0", "0", "0", "0"
            w_30m, w_1hr = 0, 0
            
            if distance == 1000:
                w_1k = format_seconds_to_string(time_seconds)
            elif distance == 2000:
                w_2k = format_seconds_to_string(time_seconds)
            elif distance == 5000:
                w_5k = format_seconds_to_string(time_seconds)
            elif distance == 10000:
                w_10k = format_seconds_to_string(time_seconds)
            elif distance == 42195:
                marathons_count += 1
                
            if raw_time_tenths == 18000:
                w_30m = distance
            elif raw_time_tenths == 36000:
                w_1hr = distance

            comment_1k = parse_pr_from_comment(comment, "1k")
            comment_2k = parse_pr_from_comment(comment, "2k")
            comment_5k = parse_pr_from_comment(comment, "5k")
            comment_10k = parse_pr_from_comment(comment, "10k")
            
            if comment_1k: w_1k = comment_1k
            if comment_2k: w_2k = comment_2k
            if comment_5k: w_5k = comment_5k
            if comment_10k: w_10k = comment_10k

            comment_30m = parse_pr_from_comment(comment, "30min")
            comment_1hr = parse_pr_from_comment(comment, "1hr")
            
            if comment_30m: w_30m = int(comment_30m)
            if comment_1hr: w_1hr = int(comment_1hr)

            if w_1k != "0": raw_1k = process_result_row_in_ram("1k", w_1k, workout_date_str, is_live_announcement)
            if w_2k != "0": raw_2k = process_result_row_in_ram("2k", w_2k, workout_date_str, is_live_announcement)
            if w_5k != "0": raw_5k = process_result_row_in_ram("5k", w_5k, workout_date_str, is_live_announcement)
            if w_10k != "0": raw_10k = process_result_row_in_ram("10k", w_10k, workout_date_str, is_live_announcement)
            
            if w_30m > 0: raw_30m = int(process_time_pr_in_ram("30min", w_30m, workout_date_str, is_live_announcement).replace(',', '').replace('m', ''))
            if w_1hr > 0: raw_1hr = int(process_time_pr_in_ram("1hr", w_1hr, workout_date_str, is_live_announcement).replace(',', '').replace('m', ''))

        # CALCULATE DAILY STREAK
        daily_streak = 0
        today_date = datetime.now()
        today_str = today_date.strftime("%Y-%m-%d")
        yesterday_str = (today_date - timedelta(days=1)).strftime("%Y-%m-%d")
        
        if today_str in rowed_dates_set:
            check_date = today_date
        elif yesterday_str in rowed_dates_set:
            check_date = today_date - timedelta(days=1)
        else:
            check_date = None
            
        while check_date is not None:
            check_str = check_date.strftime("%Y-%m-%d")
            if check_str in rowed_dates_set:
                daily_streak += 1
                check_date -= timedelta(days=1)
            else:
                break

        best_1k = RAM_PR_INDEX["1k"]["time_str"]
        best_2k = RAM_PR_INDEX["2k"]["time_str"]
        best_5k = RAM_PR_INDEX["5k"]["time_str"]
        best_10k = RAM_PR_INDEX["10k"]["time_str"]
        best_30m = f"{RAM_PR_INDEX['30min']['meters']:,}m"
        best_1hr = f"{RAM_PR_INDEX['1hr']['meters']:,}m"

        display_payload = {
            "lifetime": f"{lifetime_meters:,}m",
            "season": f"{season_meters:,}m",
            "pr_1k": best_1k if best_1k != "0" else "N/A",
            "pr_2k": best_2k if best_2k != "0" else "N/A",
            "pr_5k": best_5k if best_5k != "0" else "N/A",
            "pr_10k": best_10k if best_10k != "0" else "N/A",
            "pr_30m": best_30m if best_30m != "0m" else "N/A",
            "pr_1hr": best_1hr if best_1hr != "0m" else "N/A",
            "marathons": str(marathons_count),
            "daily_streak": str(daily_streak)
        }
        
        with open(JSON_OUTPUT_PATH, 'w') as json_file:
            json.dump(display_payload, json_file, indent=2)
            
        print(f"[SUCCESS] Grid payload fully rebuilt. Lifetime: {lifetime_meters:,}m | Daily Streak: {daily_streak} days")

    except Exception as e:
        print(f"[CRITICAL FAILURE] Pipeline execution broken: {e}")
