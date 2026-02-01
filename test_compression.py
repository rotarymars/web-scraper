#!/usr/bin/env python3
"""
Test the ZIP compression functionality for URL storage
"""
import os
import json
from scraper import save_state, load_state, STATE_FILE, _save_chunk_to_zip, _load_chunk_from_zip

def test_compression():
    """Test that ZIP compression works correctly."""
    print("Testing ZIP compression...")
    
    # Create test data with a significant amount of URLs
    visited = set([f'https://example.com/page{i}' for i in range(10000)])
    to_visit = set([f'https://test.com/url{i}' for i in range(10000)])
    start_url = "https://example.com"
    pages_scraped = 100
    urls_found = 20000
    elapsed_time = 123.45
    
    # Save state
    print("1. Saving state with ZIP compression...")
    save_state(visited, to_visit, start_url, pages_scraped, urls_found, elapsed_time)
    
    # Check that ZIP files were created
    import glob
    zip_files = glob.glob("state/*.zip")
    print(f"   ✓ Created {len(zip_files)} ZIP files")
    
    # Check file sizes
    for zip_file in zip_files:
        size_kb = os.path.getsize(zip_file) / 1024
        print(f"   ✓ {zip_file}: {size_kb:.2f} KB")
        # Ensure files are reasonably small
        assert size_kb < 1024, f"ZIP file {zip_file} is too large!"
    
    # Load state
    print("2. Loading state from ZIP files...")
    loaded_state = load_state()
    
    if loaded_state is None:
        print("   ✗ Failed to load state!")
        return False
    
    print(f"   ✓ State loaded successfully")
    
    # Verify data
    print("3. Verifying data integrity...")
    checks = [
        (loaded_state['visited_urls'] == visited, "visited_urls"),
        (loaded_state['to_visit_urls'] == to_visit, "to_visit_urls"),
        (loaded_state['start_url'] == start_url, "start_url"),
        (loaded_state['pages_scraped'] == pages_scraped, "pages_scraped"),
        (loaded_state['urls_found'] == urls_found, "urls_found"),
        (loaded_state['elapsed_time'] == elapsed_time, "elapsed_time"),
    ]
    
    all_passed = True
    for passed, field in checks:
        if passed:
            print(f"   ✓ {field} matches")
        else:
            print(f"   ✗ {field} does not match!")
            all_passed = False
    
    # Test direct compression/decompression
    print("4. Testing direct chunk compression...")
    test_chunk = [f'https://direct.test/url{i}' for i in range(1000)]
    test_zip = "/tmp/test_chunk.zip"
    
    _save_chunk_to_zip(test_chunk, test_zip)
    loaded_chunk = _load_chunk_from_zip(test_zip)
    
    if test_chunk == loaded_chunk:
        print(f"   ✓ Direct compression/decompression works")
    else:
        print(f"   ✗ Direct compression/decompression failed!")
        all_passed = False
    
    os.remove(test_zip)
    
    # Clean up
    print("\n5. Cleaning up...")
    if os.path.exists(STATE_FILE):
        os.remove(STATE_FILE)
        print(f"   ✓ Removed test state file")
    
    # Clean up chunk files
    chunk_files = glob.glob("state/scraper_state_*.zip")
    for chunk_file in chunk_files:
        if os.path.exists(chunk_file):
            os.remove(chunk_file)
    if chunk_files:
        print(f"   ✓ Removed {len(chunk_files)} chunk files")
    
    if all_passed:
        print("\n✓ All compression tests passed!")
        return True
    else:
        print("\n✗ Some tests failed!")
        return False

if __name__ == "__main__":
    print("=" * 80)
    print("ZIP Compression Test")
    print("=" * 80)
    print()
    
    success = test_compression()
    
    print()
    print("=" * 80)
    exit(0 if success else 1)
