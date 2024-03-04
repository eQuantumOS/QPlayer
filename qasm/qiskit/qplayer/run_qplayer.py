#######################################################################
#   run_qplayer.py 
#   
#   Created: 2023. 12. 13
#   Last Modified: 2023. 12. 14
#
#   Authors:
#      Sengthai Heng (sengthai@pukyong.ac.kr)
#      Nagyeong Choi (choi2019@pukyong.ac.kr)
#
#   Quantum Computing Laboratory, PKNU (quantum.pknu.ac.kr)
#######################################################################

import qiskit
from qiskit import qasm2
from qiskit import QuantumCircuit
from packaging import version
import qplayer_wra
import json
from .QResult import QResult

def executeQASM(qc : QuantumCircuit, shots:int = 1080):
    """
    Executes the given QASM code using the qplayer_wra module.

    Args:
        qasm_code (str): The QASM code to be executed.
        shots (int): The run number.

    Returns:
        QResult: The result of the QASM execution.
    """

    qiskit_version = qiskit.__version__
    if version.parse(qiskit_version) >= version.parse("1.0.1"):
        qasm_code = qasm2.dumps(qc)
    elif version.parse(qiskit_version) < version.parse("1.0.1"):
        qasm_code = qc.qasm()
    result = qplayer_wra.executeQASM(qasm_code, shots)
    result = json.loads(result)

    try:
        result = QResult(**result)
    except Exception as e:
        print(f"Error creating QResult: {e}")
        return None
    
    return result
