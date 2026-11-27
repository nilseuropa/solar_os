"""Use the firmware's cJSON source or the explicit CI checkout."""
import os
from pathlib import Path

IDF = Path(os.environ.get('IDF_PATH', str(Path.home() / '.platformio/packages/framework-espidf')))
CJSON = Path(os.environ.get('CJSON_DIR', str(IDF / 'components/json/cJSON')))
