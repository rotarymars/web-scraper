"""Submit URLs from scraped.txt to the Wayback Machine one by one."""
import time
from pathlib import Path

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.support.ui import WebDriverWait

SAVE_URL = "https://web.archive.org/save"
INPUT_ID = "web-save-url-input"
BUTTON_CLASS = "web-save-button"  # first class is enough for CSS selector
SCRAPED_FILE = Path(__file__).parent / "scraped.txt"
WAIT_BETWEEN = 1  # seconds between submissions


def main() -> None:
    urls = [line.strip() for line in SCRAPED_FILE.read_text().splitlines() if line.strip()]
    print(f"Found {len(urls)} URLs to archive")

    driver = webdriver.Chrome()
    driver.get(SAVE_URL)

    time.sleep(60)
    for i, url in enumerate(urls, 1):
        print(f"[{i}/{len(urls)}] Archiving: {url}")
        wait = WebDriverWait(driver, 30)
        inp = wait.until(EC.presence_of_element_located((By.ID, INPUT_ID)))
        inp.clear()
        inp.send_keys(url)
        btn = driver.find_element(By.CSS_SELECTOR, f".{BUTTON_CLASS}")
        btn.click()
        # Wait for page to reload/navigate before submitting the next URL
        time.sleep(WAIT_BETWEEN)
        driver.get(SAVE_URL)
        time.sleep(5)

    print("Done.")
    driver.quit()


if __name__ == "__main__":
    main()
