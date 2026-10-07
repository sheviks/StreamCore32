import sys, glob, os
from PIL import Image
d=sys.argv[1]; s=int(sys.argv[2]) if len(sys.argv)>2 else 2
for f in sorted(glob.glob(d+'/*.pgm')):
    im=Image.open(f).convert('L'); im=im.resize((im.width*s,im.height*s),Image.NEAREST)
    im.save(f[:-4]+'.png')
