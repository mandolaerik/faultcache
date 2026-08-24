# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

import os

PAGE_SIZE = 65536 if os.name == "nt" else 4096
