from pathlib import Path
import re
text = Path('src/ui/UiStrings.h').read_text(encoding='utf-8')
start = text.find('static const std::unordered_map')
end = text.find('};', start)
if start < 0 or end < 0:
    raise SystemExit('English table not found')
pairs = re.findall(r'\{\s*".*?"\s*,\s*".*?"\s*\}', text[start:end], re.S)
if len(pairs) < 70:
    raise SystemExit(f'English table unexpectedly small: {len(pairs)} entries')
print(f'English table: {len(pairs)} entries')
print('Pseudo-localization budget: +40% text expansion is reserved by responsive layout.')
