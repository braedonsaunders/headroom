import json, sys
cards = json.load(open(sys.argv[1]))
prov = {
 "G12M25_8": ("CELESTION_G12M25_8", "Celestion G12M-25 Greenback 8 ohm. S: Re 6.7, Fs 75 Hz (Celestion datasheet). M: Mms 29.22 g, Bl 12.14 T*m, Qms 9, Le 0.41 mH, Sd 525.1 cm2, Xmax 1.2 mm (measured set posted by 'fyl', Music Electronics Forum 2010-05-18, which read Fs 85 Hz). D: Kms refit to the datasheet Fs; LR-2 coil split L2 = 2 Le, R2 = 3 Re fitted to Le(1 kHz). A: Bl(x) from a 6 mm gap / 8.4 mm coil with exponential fringes (front 0.25, back 0.4 gap); Kms(x) 133 % at 1.5 Xmax, 300 % at 2.5 Xmax, +5 % asymmetry; Le(x) -15 %/Xmax (Klippel-typical shapes); thermal 110 K at rated 25 W (70 % coil, 30 % magnet), tau 8 s / 15 min"),
 "G12M25_16": ("CELESTION_G12M25_16", "Celestion G12M-25 Greenback 16 ohm. S: Re 13.1, Fs 75 Hz (Celestion datasheet). D: the 8 ohm card's cone and motor with the coil scaled to Re 13.1 (Bl x sqrt(13.1/6.7), Le x 13.1/6.7, so Qes is unchanged). Otherwise as CELESTION_G12M25_8"),
 "V30_16": ("CELESTION_V30_16", "Celestion Vintage 30 16 ohm. S: Re 12.9, Fs 75 Hz (Celestion datasheet). M: Mms 29 g, Bl 16.4 T*m, Qms 9.2 (a forum measurement attributed to the V30 only by its Re of 12.91 ohm). A: Le(1 kHz) 0.8 mH, Sd, motor geometry and nonlinear shapes as the G12M (same 44 mm coil family); thermal 110 K at rated 60 W"),
 "P12Q_8": ("JENSEN_P12Q_8", "Jensen P12Q 8 ohm (Jensen specification sheet, S): Re 5.6, Fs 90.5 Hz, Qms 11.57, Mms 26.8 g, Bl 5.88 T*m, Le 0.67 mH, Sd 490.9 cm2, Xmax 1 mm, winding depth 8 mm, gap depth 6 mm (the sheet's Qes 3.33 is inconsistent with its Qms/Qts; this card's Qes is 2.47 from Bl, Mms, Re). A: shapes as CELESTION_G12M25_8 from that geometry; thermal 110 K at rated 40 W"),
 "C12N_8": ("JENSEN_C12N_8", "Jensen C12N 8 ohm (Jensen specification sheet, S): Re 6.05, Fs 113 Hz, Qms 7.52, Mms 29.9 g, Bl 10.46 T*m, Le 0.9 mH, Sd 490.9 cm2, Xmax 1 mm, winding depth 10 mm, gap depth 8 mm. A: shapes from that geometry; thermal 110 K at rated 50 W"),
 "LEGEND1258_8": ("EMINENCE_LEGEND1258_8", "Eminence Legend 1258 8 ohm (Eminence datasheet, S): Re 7.44, Fs 94 Hz, Qms 6.15, Mms 32 g, Bl 10.9 T*m, Le 0.70 mH, Sd 506.7 cm2, Xmax 0.5 mm, gap height 7.92 mm. A: shapes from that geometry; thermal 110 K at rated 75 W"),
}
keys = ["re","le","l2","r2","bl","mms","kms","rms","sd","b1","b2","b3","b4","k1","k2","k4","lx1","lx2","alpha","rtv","ctv","rtm","ctm"]
for k, (nm, pv) in prov.items():
    c = cards[k]
    ps = ", ".join('{"%s", %s}' % (q, ('%.6g' % c[q])) for q in keys)
    print('            {"%s", "speaker", {%s},\n             "%s"},' % (nm, ps, pv))
