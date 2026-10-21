import argparse
import os
from pathlib import Path
import sys

parser=argparse.ArgumentParser()
parser.add_argument('--output', required=True)
args=parser.parse_args()
engine=Path(os.environ['HE_ENGINE_ROOT'])
sys.path.insert(0,str(engine/'Tools/Build'))
from ExportPortableView import export
export(Path(__file__).resolve().parents[1], args.output)
