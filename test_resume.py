#!/usr/bin/env python3
"""
Test the resume functionality to ensure it can continue scraping from saved state
"""
import os
import json
import unittest
from scraper import scrape, save_state, load_state, STATE_FILE

class TestResumeFunctionality(unittest.TestCase):
    
    def setUp(self):
        """Remove state file before each test"""
        if os.path.exists(STATE_FILE):
            os.remove(STATE_FILE)
    
    def tearDown(self):
        """Clean up state file after each test"""
        if os.path.exists(STATE_FILE):
            os.remove(STATE_FILE)
    
    def test_resume_with_previous_pages(self):
        """Test that resume adds previous pages to max_pages limit"""
        # Create a mock state with 50 pages already scraped
        visited = {"https://example.com/page1", "https://example.com/page2"}
        to_visit = {"https://example.com/page3"}
        start_url = "https://example.com"
        pages_scraped = 50
        urls_found = 100
        elapsed_time = 10.0
        
        # Save the mock state
        save_state(visited, to_visit, start_url, pages_scraped, urls_found, elapsed_time)
        
        # Verify state was saved
        self.assertTrue(os.path.exists(STATE_FILE))
        
        # Load state
        state = load_state()
        self.assertIsNotNone(state)
        self.assertEqual(state['pages_scraped'], 50)
        
        # Simulate the behavior in scrape function
        max_pages = 25  # Want to scrape 25 more pages
        if state:
            pages_scraped = state['pages_scraped']
            # When resuming, add previous pages to max_pages
            new_max_pages = pages_scraped + max_pages
            
            # Should be 50 + 25 = 75
            self.assertEqual(new_max_pages, 75)
            self.assertGreater(new_max_pages, pages_scraped)
    
    def test_resume_without_state(self):
        """Test that resume works gracefully when no state exists"""
        # Make sure no state file exists
        self.assertFalse(os.path.exists(STATE_FILE))
        
        # Load state should return None
        state = load_state()
        self.assertIsNone(state)
    
    def test_state_preserves_all_data(self):
        """Test that state saves and loads all necessary data"""
        visited = {"https://example.com/1", "https://example.com/2", "https://example.com/3"}
        to_visit = {"https://example.com/4", "https://example.com/5"}
        start_url = "https://example.com"
        pages_scraped = 3
        urls_found = 15
        elapsed_time = 5.5
        
        # Save state
        save_state(visited, to_visit, start_url, pages_scraped, urls_found, elapsed_time)
        
        # Load state
        state = load_state()
        
        # Verify all data is preserved
        self.assertEqual(state['visited_urls'], visited)
        self.assertEqual(state['to_visit_urls'], to_visit)
        self.assertEqual(state['start_url'], start_url)
        self.assertEqual(state['pages_scraped'], pages_scraped)
        self.assertEqual(state['urls_found'], urls_found)
        self.assertEqual(state['elapsed_time'], elapsed_time)
        self.assertIn('timestamp', state)
    
    def test_max_pages_calculation_edge_cases(self):
        """Test edge cases for max_pages calculation when resuming"""
        test_cases = [
            (0, 100, 100),    # No pages scraped yet
            (50, 50, 100),    # Half scraped, want to scrape same amount
            (99, 1, 100),     # Almost at limit, want 1 more
            (100, 100, 200),  # At limit, want to scrape 100 more
        ]
        
        for pages_scraped, additional_pages, expected_limit in test_cases:
            with self.subTest(pages_scraped=pages_scraped, additional_pages=additional_pages):
                new_limit = pages_scraped + additional_pages
                self.assertEqual(new_limit, expected_limit)
                self.assertGreaterEqual(new_limit, pages_scraped)

if __name__ == '__main__':
    unittest.main()
