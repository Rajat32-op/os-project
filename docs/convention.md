# Coding Conventions Used

This project follows the naming conventions below:

## Class Names
- **PascalCase** (StartFromCapital)
- Example:
  - `CpuMonitor`
  - `FrequencyController`

## Function Names
- **camelCase**
- Example:
  - `updateMetrics()`
  - `readEnergyData()`

## Variable Names
- **snake_case**
- Example:
  - `core_count`
  - `prev_energy_uj`
  - `current_frequency`

These conventions are used consistently across the entire codebase for clarity and maintainability.

# File convention used

## making functions importable

Use .h file + .cpp file. wherever that function is needed include the .h file.
dont do forward declaration