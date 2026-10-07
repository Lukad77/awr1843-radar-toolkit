const $ = id => document.getElementById(id);
let token = '', pending = false, connected = false, latest = {state:'idle'};
const names = {idle:'待采集',starting:'正在配置',recording:'采集中',stopping:'正在保存',completed:'已完成',failed:'采集异常'};
const tips = {idle:'等待开始',starting:'正在打开设备并下发 CFG，请稍候。',recording:'正在接收并保存雷达原始数据。',stopping:'正在停止设备并写入统计，请勿关闭服务。',completed:'本次采集已结束并保存。',failed:'请查看错误日志；已接收的数据保留在会话目录。'};
function controls(){
  const active = ['starting','recording','stopping'].includes(latest.state);
  $('start').disabled = pending || !connected || active;
  $('stop').disabled = pending || !connected || !['starting','recording'].includes(latest.state);
  document.querySelectorAll('#form input, #form select').forEach(e => e.disabled=active || pending);
  $('refreshPorts').disabled=active || pending;
  $('beltSettings').hidden=!$('belt').value;
}
async function api(path, body){
  const response = await fetch(path, body === undefined ? {cache:'no-store'} : {method:'POST',headers:{'Content-Type':'application/json','X-Capture-Token':token},body:JSON.stringify(body)});
  const data = await response.json();
  if(!response.ok) throw new Error(data.error || response.statusText);
  return data;
}
function render(s){
  latest=s; $('state').textContent=names[s.state] || s.state; $('state').dataset.state=s.state;
  $('frames').textContent=Number(s.frames).toLocaleString(); $('size').textContent=(s.bytes/1048576).toFixed(1)+' MB';
  $('statusText').textContent=tips[s.state] || '';
  if(s.directory) $('directory').textContent=s.directory;
  $('missing').textContent=s.result.missingPackets ?? '—'; $('discarded').textContent=s.result.discardedFrames ?? '—';
  $('logs').textContent=s.logs.join('\n') || '等待设备日志…';
  $('beltStatus').hidden=!s.belt;
  if(s.belt){const b=s.belt, q=b.phase_stats?.formal || {};
    const phases={prepare:'连接确认',warmup:'稳定段',ready:'等待雷达',formal:'正式采集',tail:'排空尾部',stopping:'已停止采样'};
    $('beltSummary').textContent=`${b.port || ''} · ${phases[b.phase] || b.phase} · ${b.samples || 0} 个样本 · 当前值 ${b.latest ?? '—'} · 档位 ${b.gain_commanded ?? '—'}`;
    $('beltRails').textContent=q.rail_hits ?? 0; $('beltRange').textContent=q.out_of_range ?? 0; $('beltChecks').textContent=b.checksum_errors ?? 0;
    $('beltQuality').textContent=b.error || ((q.rail_hits || q.out_of_range || b.checksum_errors || b.length_errors) ? '存在质量标记，请检查原始数据和分阶段统计。' : '原始值与稳定段均保留；校验通过不代表能检测所有丢样。');
  }
  if(s.error) $('message').textContent=s.error;
  controls();
}
$('form').addEventListener('submit',async e=>{
  e.preventDefault();pending=true;controls();$('message').textContent='';
  const data={}; ['cfgPath','serial','bindIp','dcaIp','belt'].forEach(k=>data[k]=$(k).value.trim());
  ['dataPort','configPort','packetDelayUs','lvdsLanes','maxFrames'].forEach(k=>data[k]=Number($(k).value));
  if(data.belt){data.beltPort=$('beltPort').value;data.beltGain=Number($('beltGain').value);}
  try{render(await api('/api/start',data));}catch(e){$('message').textContent=e.message;}finally{pending=false;controls();}
});
$('stop').addEventListener('click',async()=>{
  pending=true;controls();try{render(await api('/api/stop',{}));}catch(e){$('message').textContent=e.message;}finally{pending=false;controls();}
});
$('cfgFile').addEventListener('change',async e=>{
  const file=e.target.files[0];if(!file)return;
  pending=true;controls();
  try{if(file.size>1048576)throw new Error('CFG 文件不得超过 1 MiB。');
    const result=await api('/api/cfg',{content:await file.text()});$('cfgPath').value=result.path;
    $('message').textContent='';
  }catch(e){$('message').textContent=e.message;}finally{pending=false;controls();}
});
function setBeltPorts(result){
  const previous=$('beltPort').value;
  $('beltPort').replaceChildren(new Option('请选择已连接的 CP210x',''));
  for(const p of result.ports) $('beltPort').append(new Option(`${p.port} · ${p.description}${p.serial_number ? ' · '+p.serial_number : ''}`,p.port));
  if(result.ports.some(p=>p.port===previous)) $('beltPort').value=previous;
  else if(result.ports.length===1) $('beltPort').value=result.ports[0].port;
  $('beltNote').textContent=result.error || (result.ports.length ? '已识别 CP210x；开始时仍会读取并保存 HKH-11C 设备号。' : '未识别到呼吸带。仅雷达模式仍可使用；连接后刷新端口。');
}
$('belt').addEventListener('change',controls);
$('refreshPorts').addEventListener('click',async()=>{
  try{setBeltPorts(await api('/api/belt-ports'));}catch(e){$('message').textContent=e.message;}
});
async function poll(){
  try{if(!token){const options=await api('/api/options');token=options.token;$('cfgPath').value=options.cfgPath;$('directory').textContent=options.outputRoot;
    for(const name of options.belts){const option=document.createElement('option');option.value=name;option.textContent='雷达 + '+name;$('belt').append(option);}
    setBeltPorts(options.beltPorts || {ports:[],error:''});
  }const state=await api('/api/status');connected=true;render(state);
  }catch(e){connected=false;$('state').textContent='服务未连接';$('statusText').textContent='无法获取采集状态，请确认本地服务正在运行。';controls();}
  setTimeout(poll,700);
}
poll();
