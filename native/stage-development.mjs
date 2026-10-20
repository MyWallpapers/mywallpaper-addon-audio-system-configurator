// Local DEV packaging of an already verified Windows component binary.
// This never compiles and cannot qualify a release or application activation.
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const root=path.resolve(fileURLToPath(new URL('..',import.meta.url)));
const { collectCheckoutSourceIdentity }=await import(new URL('../../MyWallpaper/tools/dev/source-identity.mjs',import.meta.url));
const input=JSON.parse(readFileSync(path.join(root,'.mywallpaper/development-input.json'),'utf8'));
const receipt=JSON.parse(readFileSync(path.join(root,input.receipt),'utf8'));
const current=collectCheckoutSourceIdentity(root);
if(receipt.status!=='complete'||!receipt.source.unchanged||!current.sourceComplete||current.head!==receipt.source.after.head||input.sourceFingerprint!==current.sourceFingerprint||receipt.source.after.sourceFingerprint!==current.sourceFingerprint)throw new Error('DEV candidate sources changed or component proof is incomplete; rebuild and restage explicitly.');
const binary=readFileSync(input.binary);
if(createHash('sha256').update(binary).digest('hex')!==input.sha256)throw new Error('Verified Windows candidate binary changed.');
const out=path.join(root,'native/out/windows-x86_64');
mkdirSync(path.join(out,'notices'),{recursive:true});
writeFileSync(path.join(out,'companion.exe'),binary);
for(const file of ['LICENSE','THIRD_PARTY_NOTICES.md'])copyFileSync(path.join(root,file),path.join(out,file));
copyFileSync(path.join(root,'third_party/nlohmann/LICENSE.MIT'),path.join(out,'notices/nlohmann-json.LICENSE.MIT'));
console.log('Staged exact verified Windows component for DEV; application acceptance pending.');
