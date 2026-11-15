import sys
from PIL import Image
from rembg import remove, new_session

src, out_dir = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGB')
for model in sys.argv[3:]:
    s = new_session(model)
    for matting in (False, True):
        r = remove(im, session=s, alpha_matting=matting, alpha_matting_foreground_threshold=240,
                   alpha_matting_background_threshold=15, alpha_matting_erode_size=8)
        r.save(f'{out_dir}/cut_{model}{"_matting" if matting else ""}.png')
        print('wrote', model, matting)
