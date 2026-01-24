#!/usr/bin/env python3
"""
Test the state persistence functionality
"""
import os
import json
from scraper import save_state, load_state, STATE_FILE

def test_state_persistence():
    """Test that state can be saved and loaded correctly."""
    print("Testing state persistence...")
    
    # Test data
    visited = {"https://example.com/page1", "https://example.com/page2"}
    to_visit = {"https://example.com/page3", "https://example.com/page4"}
    start_url = "https://example.com"
    pages_scraped = 2
    urls_found = 10
    elapsed_time = 15.5
    
    # Save state
    print("1. Saving state...")
    save_state(visited, to_visit, start_url, pages_scraped, urls_found, elapsed_time)
    
    # Check file exists
    if os.path.exists(STATE_FILE):
        print(f"   ✓ State file created: {STATE_FILE}")
    else:
        print(f"   ✗ State file not found!")
        return False
    
    # Load state
    print("2. Loading state...")
    loaded_state = load_state()
    
    if loaded_state is None:
        print("   ✗ Failed to load state!")
        return False
    
    print(f"   ✓ State loaded successfully")
    
    # Verify data
    print("3. Verifying data...")
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
    
    # Print loaded state
    print("\n4. Loaded state contents:")
    print(f"   Start URL: {loaded_state['start_url']}")
    print(f"   Pages scraped: {loaded_state['pages_scraped']}")
    print(f"   URLs found: {loaded_state['urls_found']}")
    print(f"   Elapsed time: {loaded_state['elapsed_time']}s")
    print(f"   Visited URLs: {len(loaded_state['visited_urls'])}")
    print(f"   To-visit URLs: {len(loaded_state['to_visit_urls'])}")
    
    # Clean up
    print("\n5. Cleaning up...")
    if os.path.exists(STATE_FILE):
        os.remove(STATE_FILE)
        print(f"   ✓ Removed test state file")
    
    if all_passed:
        print("\n✓ All tests passed!")
        return True
    else:
        print("\n✗ Some tests failed!")
        return False

if __name__ == "__main__":
    print("=" * 80)
    print("State Persistence Test")
    print("=" * 80)
    print()
    
    success = test_state_persistence()
    
    print()
    print("=" * 80)
    exit(0 if success else 1)
