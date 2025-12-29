# Qore Python Module

This module provides bidirectional integration between Qore and Python, allowing:

- **Qore code to call Python**: Import Python modules, classes, and functions into Qore programs
- **Python code to call Qore**: Use the `qoreloader` module to import Qore APIs into Python programs
- **Cross-language inheritance**: Qore classes can inherit Python classes and vice-versa

## Requirements

- Qore 1.0+
- Python 3.7 - 3.11
- CMake 3.5+
- C++11 compatible compiler

## Building

```bash
mkdir build && cd build
cmake ..
make
make install
```

## Quick Start

### Using Python from Qore

```qore
%requires python

# Import Python's math module
%module-cmd(python) import math

# Use Python functions in Qore
float result = math::sin(1.0);
```

### Using Qore from Python

```python
import qoreloader
from qore.SqlUtil import Table

# Use Qore classes in Python
t = Table("pgsql:user/pass@db", "my_table")
```

## Documentation

Full documentation is available in the module's Doxygen documentation, generated during the build process.

## License

MIT License - see COPYING.MIT for details.

## Links

- [Qore Programming Language](https://qore.org)
- [Issue Tracker](https://github.com/qorelanguage/qore/issues)
