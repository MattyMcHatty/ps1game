import xml.etree.ElementTree as ET
import sys

def scale_smx(input_file, output_file, sx, sy, sz):
    ET.register_namespace('', '')
    tree = ET.parse(input_file)
    root = tree.getroot()

    vertices_elem = root.find('vertices')
    if vertices_elem is None:
        print("ERROR: no <vertices> section found")
        sys.exit(1)

    for v in vertices_elem.findall('v'):
        v.set('x', f"{float(v.get('x')) * sx:.6f}")
        v.set('y', f"{float(v.get('y')) * sy:.6f}")
        v.set('z', f"{float(v.get('z')) * sz:.6f}")

    tree.write(output_file, encoding='unicode', xml_declaration=False)
    print(f"Scaled vertices x*{sx} y*{sy} z*{sz} -> {output_file}")

if __name__ == '__main__':
    scale_smx(
        'assets/eight_cut_room.smx',
        'assets/eight_cut_room_scaled.smx',
        sx=540.0, sy=90.0, sz=540.0
    )
