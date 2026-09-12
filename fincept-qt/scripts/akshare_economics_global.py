"""
AKShare Global Economics Data Wrapper
Wrapper for global economic indicators
Returns JSON output for Qt/C++ integration

NOTE: All 45 endpoints timeout or fail. File deprecated.
All macro_usa_* and macro_bank_* endpoints are non-functional.
"""

import sys
import json
from datetime import datetime

# Keep the deprecation notice on the __main__ path only. This module used to
# print the notice and call sys.exit(1) at import time, which killed any
# process that imported it — akshare_data.py does (in a try/except ImportError
# that never saw the SystemExit), so every akshare_data.py endpoint failed with
# this stub's exit before doing any work.
if __name__ == "__main__":
    print(json.dumps({
        "success": False,
        "error": "akshare_economics_global.py deprecated - all 45 endpoints non-functional",
        "data": [],
        "count": 0,
        "timestamp": int(datetime.now().timestamp())
    }))
    sys.exit(1)
