# ModuOS AI Agent Configuration and Coding Standards

> [!WARNING]
> **RESTRICTED USE ADVISORY**: This project is intended for manual development. AI assistance should only be utilized when absolutely necessary. Human oversight is mandatory for all architectural decisions.

---

## AI Agent Operational Rules

To ensure project integrity and stability, all AI agents must strictly adhere to the following directives:

1.  **Build Process**: Do not attempt to rebuild the project. The user is responsible for all build operations.
2.  **Make Commands**: Execution of the `make` command is strictly prohibited.
3.  **Build Configuration**: Do not modify the `Makefile` without explicit user confirmation.
4.  **Technical Respect**: Do not explain basic C or Assembly syntax. Assume the user is an expert. Document only the *intent* of complex logic or non-obvious hardware interactions. Do not provide comments for trivial code (e.g., loops, assignments, or standard register moves).
5.  **Communication**: Maintain a professional, peer-to-peer tone. Avoid alarmist keywords like "CRITICAL" or "EMERGENCY."
6.  **Environment**: Do not create temporary or auxiliary files.
7.  **Implementation**: Produce professional-grade code adhering to the standards below.

---

# Coding Standards

## General Principles

1.  **Clarity over Cleverness**: Prioritize readability except in *safety-critical* paths (fault handling, paging, context switching), where **robustness and correctness** are paramount.
2.  **Consistency**: Adhere to established architectural patterns.
3.  **Documentation**: Explain *why* a design choice was made, not *what* the code is doing on a line-by-line basis.

## C Programming Style

### Naming Conventions
* **Functions**: `subsystem_action_object()` (e.g., `process_create`).
* **Variables**: Local variables use `snake_case`. Globals must be subsystem-prefixed.
* **Types**: Suffix structs, enums, and typedefs with `_t`.

### Formatting
* **Indentation**: 4 spaces (no tabs).
* **Braces**: K&R style.
* **Line Length**: Prefer 80 characters, maximum 100.

## Hardware Driver Guidelines

### Interrupt Initialization Sequence

To prevent interrupt storms during initialization:
1.  Clear and disable hardware interrupts.
2.  **Mandatory**: Install the IRQ handler **before** enabling hardware interrupts.
3.  Clear pending interrupt status before and after handler installation.
4.  Use "posted reads" after critical register writes to ensure PCI transaction completion.

## Assembly Style (NASM)

* **Syntax**: 64-bit NASM.
* **Documentation**: Define parameters (e.g., `RDI`) and return values (`RAX`).
* **Formatting**: Instructions indented 4 spaces; labels at 0.

---

## Tooling and Analysis

* **Formatting**: `clang-format` (LLVM-based).
* **Static Analysis**: Compile with `-Wall -Wextra -Werror`.