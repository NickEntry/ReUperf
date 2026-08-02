# Third-party notices

## nlohmann/json v3.11.3

- Source: <https://github.com/nlohmann/json>
- License: MIT License
- Use: JSON parsing and structured logging.

The full license text for nlohmann/json is available in its source distribution at
<https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT>.

## Magisk compatibility interface

ReUperf does not copy or bundle Magisk's GPL-licensed `scripts/module_installer.sh`.
The repository-local `module_template/META-INF/com/google/android/update-binary` is an
independent MIT-licensed entry point that invokes the `util_functions.sh` supplied by the
user's installed Magisk environment through its documented module installation interface.
Magisk itself is licensed under GPL-3.0-or-later and is not distributed by this repository.
