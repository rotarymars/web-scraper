#!/usr/bin/env python3
"""
Unit tests for the web scraper
"""
import unittest
from scraper import extract_urls

class TestWebScraper(unittest.TestCase):
    
    def test_extract_urls_with_href(self):
        """Test URL extraction from href attributes"""
        html = '''
        <html>
            <a href="https://example.com">Link 1</a>
            <a href="http://test.com">Link 2</a>
            <a href="/relative/path">Link 3</a>
        </html>
        '''
        base_url = "https://example.com"
        urls = extract_urls(html, base_url)
        
        self.assertIn("https://example.com", urls)
        self.assertIn("http://test.com", urls)
        self.assertIn("https://example.com/relative/path", urls)
    
    def test_extract_urls_with_src(self):
        """Test URL extraction from src attributes"""
        html = '''
        <html>
            <img src="https://example.com/image.jpg">
            <script src="/js/script.js"></script>
        </html>
        '''
        base_url = "https://example.com"
        urls = extract_urls(html, base_url)
        
        self.assertIn("https://example.com/image.jpg", urls)
        self.assertIn("https://example.com/js/script.js", urls)
    
    def test_extract_direct_urls(self):
        """Test extraction of direct URLs in content"""
        html = '''
        <html>
            Check out https://example.com and http://test.org for more info.
        </html>
        '''
        base_url = "https://example.com"
        urls = extract_urls(html, base_url)
        
        self.assertIn("https://example.com", urls)
        self.assertIn("http://test.org", urls)
    
    def test_extract_urls_relative_to_absolute(self):
        """Test conversion of relative URLs to absolute"""
        html = '<a href="/page1">Page 1</a>'
        base_url = "https://example.com/dir/"
        urls = extract_urls(html, base_url)
        
        self.assertIn("https://example.com/page1", urls)
    
    def test_extract_urls_ignores_non_http(self):
        """Test that non-http(s) URLs are ignored"""
        html = '''
        <html>
            <a href="ftp://example.com/file">FTP</a>
            <a href="mailto:test@example.com">Email</a>
            <a href="javascript:void(0)">JS</a>
            <a href="https://example.com">Valid</a>
        </html>
        '''
        base_url = "https://example.com"
        urls = extract_urls(html, base_url)
        
        # Should only contain the https URL
        self.assertIn("https://example.com", urls)
        # Should not contain ftp, mailto, or javascript
        for url in urls:
            self.assertTrue(url.startswith('http://') or url.startswith('https://'))
    
    def test_extract_urls_returns_set(self):
        """Test that extract_urls returns a set (no duplicates)"""
        html = '''
        <html>
            <a href="https://example.com">Link 1</a>
            <a href="https://example.com">Link 2</a>
            <a href="https://example.com">Link 3</a>
        </html>
        '''
        base_url = "https://example.com"
        urls = extract_urls(html, base_url)
        
        self.assertIsInstance(urls, set)
        # Even though the URL appears 3 times, it should only be in the set once
        self.assertIn("https://example.com", urls)
        # Check that there's only one unique URL in the result
        self.assertEqual(len([u for u in urls if u == "https://example.com"]), 1)

if __name__ == '__main__':
    unittest.main()
