![image](https://github.com/eQuantumOS/QPlayer/assets/17871602/3e9517cf-bed6-45b9-8cea-255bf1b84935)

# v2.4-Cougar
+ release date : March 4, 2024
+ features
  - optimize toffoli gate
  - support qiskit-compatible python interface
  - reorganize ~/qasm directory
    * ~/qasm/cli    : CLI utility for OpenQASM code
    * ~/qasm/qiskit : python interface
  
# v2.0-Puma
+ release date : August 31, 2023
+ features
  - approximately 2 to 10 times improved calculation performance thana
    v1.0-Leopard by utilizing gate-aware low computational matrix calculation 
    * DiagonalGates() : Z, S, T, SDG, TDG, RZ, U1
    * NoneDiagonalGates() : X, Y, H, U2, U3, RX, RY, SX
    * DiagonalControlGates() : CZ, CU1, CRZ
    * NoneDiagonalControlGates() : CX, CY, CU2, CU3, CH, CSWAP
  - remove locking on access to multiple state containers
  - refine the testing directory as follows
    * ~/test/general
    * ~/test/surfacecode
    * ~/test/verification
    * ~/test/benchmark

# v1.0-Leopard
+ release date : June 23, 2023
+ features
  - re-organize whole directory trees & sources
  - OpenQASM v2.0 /w a complete grammar check
  - add state memory pool for handling dynamical state memory
  - provide simulation statistics through the json file

# v0.5-Jaguar 
+ release date : March 30, 2023
+ features
  - optimize matrix calculation logic
  - fix memory overflow in log-run testing
  - enhance gate operation performance
  - OpenQASM v2.0 /w a simple grammar check
  - Support for 32 quantum gate operations
 
# v0.2-Cat
+ release date : June 24, 2021
+ features
  - Schrödinger style state vector simulator  
  - Fast operation in the reduced Hilbert space
  - User-friendly interfaces: native-C
  - Support for 21 quantum gate operations
