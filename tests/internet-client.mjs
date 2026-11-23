import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {once} from 'node:events';
import {readFile} from 'node:fs/promises';
import {join,resolve} from 'node:path';
import {createInterface} from 'node:readline';
import {setTimeout as delay} from 'node:timers/promises';

const [directory, probePath, invitationFile, returnInviteFile] = process.argv.slice(2);
const folder = resolve(directory);
const invite = (await readFile(invitationFile,'utf8')).trim();
const code = invite.split('.').at(-1);
const children = [];
async function joinGuest(name) {
    const child = spawn(join(folder,'kanal-relay.exe'),['join',invite],{cwd:folder,stdio:['pipe','pipe','inherit'],windowsHide:true});
    children.push(child);
    const reader = createInterface({input:child.stdout});
    const [line] = await once(reader,'line',{signal:AbortSignal.timeout(35000)});
    reader.close();
    const data = JSON.parse(line);
    assert.ok(data.turnPort && data.turnHost,data.error || 'Expected the Internet voice path');
    let key = '';
    async function request(op,fields={}) {
        const response=await fetch(data.address+'/api',{method:'POST',headers:{'Content-Type':'application/json',...(key&&{Authorization:'Bearer '+key})},
            body:JSON.stringify({op,...fields}),signal:AbortSignal.timeout(22000)});
        const result=await response.json();
        assert.ok(!result.error,result.error);
        return result;
    }
    key=(await request('register',{name,color:1})).key;
    await request('join',{code});
    const voice=await request('voice',{code,room:'TEST000VOICE'});
    return {request,token:voice.token,url:data.address.replace(/^http:/,'ws:'),turn:[data.turnHost+':'+data.turnPort,voice.turnUser,voice.turnPassword]};
}

try {
    const [a,b]=await Promise.all([joinGuest('Remote Test A'),joinGuest('Remote Test B')]);
    const nonce=randomBytes(8).toString('hex');
    await a.request('chat',{code,room:'TEST0000CHAT',text:'verify '+nonce});
    let acknowledged=false;
    const end=Date.now()+30000;
    while(!acknowledged && Date.now()<end) {
        const result=await b.request('poll');
        acknowledged=(result.events || []).some(e=>e.text==='ack '+nonce);
    }
    assert.ok(acknowledged,'Missing chat response from the other network');
    console.log('Chat round trip through the independent host passed');
    const audio=spawn(resolve(probePath),[a.url,a.token,b.token,'32',b.url,...b.turn,...a.turn],
        {cwd:folder,stdio:'inherit',windowsHide:true});
    try {
        const [status]=await once(audio,'exit',{signal:AbortSignal.timeout(65000)});
        assert.equal(status,0,'Duplex audio across the Internet');
    } finally {
        if(audio.exitCode===null && !audio.signalCode) audio.kill();
    }
    await a.request('chat',{code,room:'TEST0000CHAT',text:'duplex audio verified'});
    if (returnInviteFile) {
        await delay(250);
        const offer=(await readFile(returnInviteFile,'utf8')).trim();
        await a.request('chat',{code,room:'TEST0000CHAT',text:'return '+offer});
        let returned=false;
        const until=Date.now()+100000;
        while(!returned && Date.now()<until) {
            const result=await b.request('poll');
            returned=(result.events||[]).some(e=>e.text==='return received');
        }
        assert.ok(returned,'The independent host did not receive the home test invite');
        await b.request('chat',{code,room:'TEST0000CHAT',text:'return acknowledged'});
        console.log('The independent host received the invite for the return test');
    }
    console.log('PASS: Internet invite, text and duplex audio on separate networks');
} finally {
    for(const child of children) child.stdin.end();
}
