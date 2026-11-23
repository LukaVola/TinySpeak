// A disposable host for testing a Windows client from a separate network.
// Only the encrypted endpoint and a temporary server tag appear in the test invite.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { writeFile, mkdir } from 'node:fs/promises';
import { resolve, join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createInterface } from 'node:readline';

const [binary, directory, returnProbe] = process.argv.slice(2);
if (!binary || !directory) throw new Error('Expected a host executable and a temporary test directory');
const profile = join(directory, 'profile');
await mkdir(profile, {recursive:true});
const child = spawn(resolve(binary), ['host', profile], {stdio:['pipe','pipe','inherit'],windowsHide:true});
const reader = createInterface({input:child.stdout});
const secret = randomBytes(20).toString('hex');
const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
let code = '';
for (let n = 0; code.length < 16; n++) {
    const block = createHash('sha256').update(`Kanal invite v1:${secret}:${n}`).digest();
    for (const b of block) {
        if (b < 248) code += alphabet[b % 62];
        if (code.length === 16) break;
    }
}
try {
    const [line] = await once(reader, 'line', {signal:AbortSignal.timeout(35000)});
    const host = JSON.parse(line);
    assert.ok(host.internet && host.ticket, 'The independent host could not reach its public relay');
    let key = '';
    async function request(op, fields = {}) {
        const res = await fetch(host.address + '/api', {method:'POST',headers:{
            'Content-Type':'application/json', ...(key && {Authorization:'Bearer '+key}),
        },body:JSON.stringify({op,...fields}),signal:AbortSignal.timeout(20000)});
        const reply = await res.json();
        assert.ok(!reply.error,reply.error);
        return reply;
    }
    key = (await request('register',{name:'Network Test Host',color:0})).key;
    await request('advertise',{code,secret,name:'Temporary Internet test'});
    const text = 'TEST0000CHAT', voice = 'TEST000VOICE';
    await request('hydrate',{code,rooms:[{id:text,kind:'text',name:'Chat'},{id:voice,kind:'voice',name:'Voice'}]});
    await request('join',{code});
    const route=Buffer.from(host.route,'base64url');
    route.fill(0,0,8);
    await writeFile(join(directory,'invite'),`TS2-${route.toString('base64url')}.${host.ticket}.${code}`);
    const end=Date.now()+5*60*1000;
    let answered = false, verified = false, returned = false, acknowledged = false;
    while ((!verified || (returnProbe && !acknowledged)) && Date.now() < end) {
        const result=await request('poll');
        for (const event of result.events || []) {
            if(event.type !== 'chat' || event.room !== text) continue;
            if(/^verify [a-f0-9]{16}$/.test(event.text)) {
                await request('chat',{code,room:text,text:'ack '+event.text.slice(7)});
                answered = true;
            }
            if(answered && event.text === 'duplex audio verified') verified = true;
            if(returned && event.text === 'return acknowledged') acknowledged = true;
            if(returnProbe && verified && !returned && event.text.startsWith('return TS2-')) {
                const invite=event.text.slice(7);
                assert.match(invite,/^TS2-[A-Za-z0-9_-]{32}\.endpoint[a-z2-7]{50,192}\.[A-Za-z0-9]{16}$/);
                assert.ok(Buffer.from(invite.slice(4,36),'base64url').subarray(0,8).every(b=>b===0),'Expected a relay-only test fixture');
                const path=join(directory,'return-invite');
                await writeFile(path,invite);
                const script=fileURLToPath(new URL('./internet-client.mjs',import.meta.url));
                const back=spawn(process.execPath,[script,dirname(resolve(binary)),resolve(returnProbe),path],{stdio:'inherit',windowsHide:true});
                const [status]=await once(back,'exit',{signal:AbortSignal.timeout(100000)});
                assert.equal(status,0,'Remote client could not use the home host');
                await request('chat',{code,room:text,text:'return verified'});
                returned=true;
            }
        }
    }
    assert.ok(verified && (!returnProbe || acknowledged),'Timed out waiting for the independent client and its two audio directions');
    await writeFile(join(directory,'passed'),'chat and duplex audio passed');
} finally {
    reader.close();
    child.stdin.end();
}
