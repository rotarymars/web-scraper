#!/usr/bin/env python3
"""
Migration script to convert existing .json chunk files to .zip format
"""
import json
import os
import glob
import zipfile
import io
from pathlib import Path

def convert_json_to_zip(json_path):
    """Convert a JSON file to ZIP format using streaming to reduce memory usage."""
    # Use Path.with_suffix() for clearer intent
    zip_path = str(Path(json_path).with_suffix('.zip'))
    
    # Read JSON
    print(f"  Converting {json_path}...")
    with open(json_path, 'r') as f:
        data = json.load(f)
    
    # Get file sizes
    json_size = os.path.getsize(json_path) / (1024*1024)
    
    # Write to ZIP using streaming to avoid large intermediate buffers
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        with zf.open('urls.json', 'w') as zip_entry:
            # Wrap the binary ZIP entry in a text stream for json.dump
            with io.TextIOWrapper(zip_entry, encoding='utf-8') as text_stream:
                json.dump(data, text_stream)
    
    zip_size = os.path.getsize(zip_path) / (1024*1024)
    
    # Handle zero-size JSON files to avoid ZeroDivisionError
    if json_size == 0:
        print(f"    JSON: {json_size:.2f} MB -> ZIP: {zip_size:.2f} MB (N/A - empty file)")
        compression_ratio = 0
    else:
        compression_ratio = (1 - zip_size / json_size) * 100
        print(f"    JSON: {json_size:.2f} MB -> ZIP: {zip_size:.2f} MB ({compression_ratio:.1f}% saved)")
    
    return zip_path, json_size, zip_size

def migrate_state_files():
    """Migrate all JSON chunk files to ZIP format."""
    print("=" * 80)
    print("Migrating State Files from JSON to ZIP")
    print("=" * 80)
    print()
    
    # Find all chunk JSON files (not the main state file)
    chunk_files = []
    chunk_files.extend(glob.glob("state/scraper_state_visited_*.json"))
    chunk_files.extend(glob.glob("state/scraper_state_to_visit_*.json"))
    
    if not chunk_files:
        print("No chunk files found to migrate.")
        print()
        return
    
    print(f"Found {len(chunk_files)} chunk files to migrate...")
    print()
    
    total_json_size = 0
    total_zip_size = 0
    converted_files = []
    
    for json_file in sorted(chunk_files):
        zip_file, json_size, zip_size = convert_json_to_zip(json_file)
        total_json_size += json_size
        total_zip_size += zip_size
        converted_files.append((json_file, zip_file))
    
    print()
    print("Conversion Summary:")
    print("-" * 80)
    print(f"Files converted: {len(converted_files)}")
    print(f"Total size before: {total_json_size:.2f} MB")
    print(f"Total size after: {total_zip_size:.2f} MB")
    
    # Handle zero-size case to avoid ZeroDivisionError
    if total_json_size == 0:
        print(f"Space saved: 0.00 MB (N/A - empty files)")
    else:
        print(f"Space saved: {total_json_size - total_zip_size:.2f} MB ({(1 - total_zip_size/total_json_size)*100:.1f}%)")
    print()
    
    # Update the main state file to reference .zip files
    state_file = "state/scraper_state.json"
    if os.path.exists(state_file):
        print("Updating main state file references...")
        with open(state_file, 'r') as f:
            state = json.load(f)
        
        # Update chunk file references using Path.with_suffix() for clarity
        if 'visited_urls_chunks' in state:
            state['visited_urls_chunks'] = [str(Path(f).with_suffix('.zip')) for f in state['visited_urls_chunks']]
        if 'to_visit_urls_chunks' in state:
            state['to_visit_urls_chunks'] = [str(Path(f).with_suffix('.zip')) for f in state['to_visit_urls_chunks']]
        
        with open(state_file, 'w') as f:
            json.dump(state, f, indent=2)
        
        print(f"  ✓ Updated {state_file}")
        print()
    
    # Ask user if they want to delete old JSON files
    print("Old JSON files can now be deleted.")
    print("The following files would be removed:")
    for json_file, _ in converted_files:
        print(f"  - {json_file}")
    print()
    
    response = input("Delete old JSON files? (yes/no): ").lower().strip()
    if response in ['yes', 'y']:
        for json_file, _ in converted_files:
            os.remove(json_file)
            print(f"  ✓ Deleted {json_file}")
        print()
        print("Migration complete! Old JSON files removed.")
    else:
        print()
        print("Migration complete! Old JSON files kept.")
        print("You can delete them manually when ready:")
        print("  rm state/scraper_state_*_*.json")
    
    print()
    print("=" * 80)

if __name__ == "__main__":
    migrate_state_files()
