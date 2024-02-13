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

import qplayer_wra
import json
from .QResult import QResult

def executeQASM(qasm_code:str = None, shots:int = 1080):
    """
    Executes the given QASM code using the qplayer_wra module.

    Args:
        qasm_code (str): The QASM code to be executed.
        shots (int): The run number.

    Returns:
        QResult: The result of the QASM execution.
    """

    result = qplayer_wra.executeQASM(qasm_code, shots)
    result = json.loads(result)

    try:
        result = QResult(**result)
    except Exception as e:
        print(f"Error creating QResult: {e}")
        return None
    
    return result
