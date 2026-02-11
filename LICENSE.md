# ModuOS License Information
**Developed by NTSoftware (New Technologies Software)**

This project uses a multi-license structure. Depending on which part of the source tree you are accessing, different terms apply.

---

## 1. ModuOS Core, MDFS, and Userland (GPLv2)
The majority of the ModuOS project is licensed under the **GNU General Public License, Version 2.0**.

* **License File:** `LICENSES/GNU_GPLv2.0.txt`
* **Components Covered:**
    * ModuOS Kernel (`src/kernel/`)
    * Userland Applications (`src/user/`)
    * **MDFS (ModularFS)** implementation and headers (`src/fs/MDFS/` and `include/moduos/fs/MDFS/`)
    * Standard Library and SQRM modules.
* **Terms:** Redistribution and modification are permitted under the terms of the GPLv2.

---

## 2. QXL Architecture Headers (Modified MIT)
The QXL Graphics Headers are licensed under a **Modified MIT License** with a mandatory attribution clause. This ensures that the architecture can be ported while credit remains with **NTSoftware**.

* **License File:** `LICENSES/NTSoftware-QXLHeaders/mod_MIT.txt`
* **Components Covered:**
    * QXL Graphics Stack Headers (`modules/QXL/include/`)
* **Attribution:** All files under this license contain a mandatory header block that must remain intact in all derivative copies.

---

## 3. License Directory Structure
For the full text of these licenses, please refer to:

- `LICENSES/GNU_GPLv2.0.txt`
- `LICENSES/NTSoftware-QXLHeaders/mod_MIT.txt`