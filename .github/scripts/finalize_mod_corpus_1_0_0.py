from pathlib import Path
import json

manifest_path = Path('docs/MOD_CORPUS_1.0.0.json')
manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
mods = manifest['mods']
ids = {item['id'] for item in mods}

extras = [
    {
        'id': 'taimuresu-spam-amplified',
        'enabled_by_default': True,
        'drive_folder_id': '14MFlkrbN6sRJwnqSTwMobRJ5NVoc_VHK',
        'file_count': 36,
        'unpacked_bytes': 589994065,
        'path_size_inventory_sha256': 'f57064b2755eff10c4c1567a3af786b40f632faaa666603db2eb7023726c783c',
        'discovery': 'drive-root-audit',
        'notes': 'Runnable Psych-family mod containing Taimuresu_overkill_mode / timeless stage.',
    },
    {
        'id': 'charts-feitos-no-flp',
        'enabled_by_default': True,
        'drive_folder_id': '1Z-g_itHvEMiPnP7XB4Z9Adhu6G4S-Xnp',
        'file_count': 2,
        'unpacked_bytes': 1459,
        'path_size_inventory_sha256': 'd0e44b598435c84211f99e044c02528a446ebad708b8b3a5596b148210309f84',
        'discovery': 'drive-root-audit',
        'notes': 'Runnable mod root with mod.json and data directory.',
    },
]
for extra in extras:
    if extra['id'] not in ids:
        mods.append(extra)

manifest['source']['inventory_basis'] = (
    'modsList.txt plus direct Drive mods-root audit for runnable unlisted content'
)
manifest['source']['excluded_non_mod_directories'] = [
    '.autochart-staging',
    'Hellbreaker Remake FLP',
]
manifest['mod_count'] = len(mods)
manifest['enabled_by_default_count'] = sum(1 for item in mods if item['enabled_by_default'])
manifest['file_count'] = sum(int(item['file_count']) for item in mods)
manifest['unpacked_bytes'] = sum(int(item['unpacked_bytes']) for item in mods)

assert manifest['mod_count'] == 29
assert manifest['enabled_by_default_count'] == 27
assert manifest['file_count'] == 19585
assert manifest['unpacked_bytes'] == 15019576859
assert len({item['drive_folder_id'] for item in mods}) == 29
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

Path('docs/MOD_CORPUS_1.0.0.md').write_text(
    '''# PulseForge 1.0.0 mod corpus\n\n'''
    '''The definitive 1.0.0 distribution contains **29 runnable mods**. Twenty-seven are recorded in the historical `modsList.txt`; a direct audit of the Drive `mods` root found two additional runnable roots that the list omitted: **`taimuresu spam amplified`** (which contains Overkill) and **`charts feitos no flp`**. The complete runnable corpus contains **19,585 files** and **15,019,576,859 unpacked bytes**.\n\n'''
    '''The public Git repository stores a deterministic inventory rather than committing roughly 15.0 GB of static content into Git history. The GitHub 1.0.0 release distributes the runnable content as separately checksummed assets sourced from the public read-only Drive corpus.\n\n'''
    '''Two direct children of the Drive `mods` root are intentionally excluded because they are not runnable engine mods: `.autochart-staging` is transient AutoChart staging data, and `Hellbreaker Remake FLP` contains production/source material such as FLP/MIDI/audio rather than an installable mod tree.\n\n'''
    '''`docs/MOD_CORPUS_1.0.0.json` records the Drive folder ID, default enablement, file count, unpacked byte count and SHA-256 of the sorted `path<TAB>size` inventory for every runnable mod. Release-package SHA-256 values are generated from the actual archives and are independent of these inventory hashes.\n\n'''
    '''SCReboot is included as `drive-pack-screboot-demo`; executable Lua is scoped to the selected mod in 1.0.0 so sibling mods cannot block its countdown. Overkill is included inside `taimuresu-spam-amplified` and has a dedicated Lua compatibility regression in the 1.0.0 engine.\n''',
    encoding='utf-8',
)


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected one match for {old!r}, found {text.count(old)}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')

replace_once(
    'docs/RELEASE_NOTES_1.0.0.md',
    '- Complete 27-pack authoritative engine mod corpus distributed as checksummed GitHub release content assets.\n',
    '- Complete 29-mod runnable engine corpus distributed as checksummed GitHub release content assets; the Drive-root audit adds `taimuresu spam amplified` and `charts feitos no flp` beyond the historical 27-entry `modsList.txt`.\n',
)
replace_once(
    'CHANGELOG.md',
    '- Distributes the complete authoritative 27-pack engine mod corpus as separately verifiable release assets instead of bloating Git history with approximately 14.4 GB of static content.\n',
    '- Distributes the complete 29-mod runnable engine corpus as separately verifiable release assets instead of bloating Git history with approximately 15.0 GB of static content; a Drive-root audit recovered two runnable mods omitted from the historical `modsList.txt`.\n',
)
replace_once(
    'mods/README.md',
    'The definitive release contains the complete engine mod corpus, but the ~14.4 GB payload is intentionally not committed into Git history. See `../docs/MOD_CORPUS_1.0.0.json` for the exact 27-pack inventory and the GitHub `v1.0.0` release for the checksummed downloadable content archives.\n',
    'The definitive release contains the complete 29-mod runnable engine corpus, but the ~15.0 GB payload is intentionally not committed into Git history. See `../docs/MOD_CORPUS_1.0.0.json` for the exact inventory and the GitHub `v1.0.0` release for the checksummed downloadable content archives.\n',
)
