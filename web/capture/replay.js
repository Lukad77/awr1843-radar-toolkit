const $=id=>document.getElementById(id);
let data=null, plots=[], playing=false, lastTime=0, cursor=0, linking=false, busy=false;
const colors={radar:'#137f9c',belt:'#d68430'};
async function api(path,body){
 const opts=body===undefined?{cache:'no-store'}:{method:'POST',headers:{'Content-Type':'application/json','X-Capture-Token':(await api('/api/options')).token},body:JSON.stringify(body)};
 const response=await fetch(path,opts);const value=await response.json();if(!response.ok)throw Error(value.error||response.statusText);return value;
}
function pause(){playing=false;$('play').textContent='播放';}
function extent(){return ReplayScope.extent(data,$('scope').value);}
function visibleSeries(values){return ReplayScope.series(data,values,$('scope').value);}
function syncScale(source){if(linking || !Number.isFinite(source.scales.x.min) || !Number.isFinite(source.scales.x.max))return;linking=true;for(const p of plots)if(p!==source)p.setScale('x',{min:source.scales.x.min,max:source.scales.x.max});linking=false;}
function markers(u,kind){
 const ctx=u.ctx,ratio=devicePixelRatio||1;
 const draw=(xs,color,dash)=>{ctx.strokeStyle=color;ctx.lineWidth=ratio*.8;ctx.setLineDash(dash.map(x=>x*ratio));for(const t of xs){if(t<u.scales.x.min||t>u.scales.x.max)continue;const x=u.valToPos(t,'x',true);ctx.beginPath();ctx.moveTo(x,u.bbox.top);ctx.lineTo(x,u.bbox.top+u.bbox.height);ctx.stroke();}};
 ctx.save();ctx.beginPath();ctx.rect(u.bbox.left,u.bbox.top,u.bbox.width,u.bbox.height);ctx.clip();
 draw(data.markers.formalWindow || [],'#437f63',[6,3]);
 if(kind==='radar'){draw(data.markers.switches,'#94a0ab',[3,4]);draw(data.markers.gaps,'#c94642',[]);}
 if(kind==='belt'){draw([...data.markers.rails,...data.markers.outOfRange],'#d4746e',[2,6]);draw(data.markers.phases.map(x=>x.time),'#aab1b8',[3,5]);}
 ctx.setLineDash([]);draw([cursor],'#344c61',[]);ctx.restore();
}
function makePlot(id,kind,series,values,ylabel){
 const el=$(id);el.replaceChildren();
 if(!values[0].length){el.textContent='当前区间没有可显示的样本。';return;}
 const p=new uPlot({width:Math.max(280,el.clientWidth),height:260,scales:{x:{time:false}},
  series:[{label:'时间 / s'},...series.map(s=>({...s,width:1.5,spanGaps:false}))],
  axes:[{label:'主机接收时间 / s',labelSize:24},{label:ylabel,size:68,labelSize:22}],
  cursor:{drag:{x:true,y:false},sync:{key:'replay-audit'}},
  hooks:{setScale:[(u,key)=>{if(key==='x')syncScale(u);}],draw:[u=>markers(u,kind)]}},values,el);
 plots.push(p);
}
function render(){
 pause();linking=true;for(const p of plots)p.destroy();plots=[];$('result').hidden=false;
 $('title').textContent=data.session;const s=data.summary;
 if(!ReplayScope.formalRange(data))$('scope').value='all';
 $('scope').querySelector('option[value="formal"]').disabled=!ReplayScope.formalRange(data);
 $('summary').textContent=`${data.state==='completed'?'采集已结束':'失败会话 / 部分数据'} · 全程：雷达 ${s.total.radarFrames} 帧 · 呼吸带 ${s.total.beltSamples} 样本 · 接收跨度 ${s.total.duration.toFixed(3)} 秒 · 换单元 ${s.switches} 次`;
 $('formalSummary').textContent=s.formal ? `正式区间 [T0,T1)：雷达 ${s.formal.radarFrames} 帧 · 呼吸带 ${s.formal.beltSamples} 样本 · 区间时长 ${s.formal.duration.toFixed(3)} 秒 · 端点 ${s.formal.railHits} · 超量程 ${s.formal.outOfRange}` : '正式区间：旧会话未记录共同 T0/T1，无法给出统一正式统计。';
 $('clockBasis').textContent=ReplayScope.clockLabel(data);
 $('warnings').replaceChildren(...data.warnings.map(text=>{const li=document.createElement('li');li.textContent=text;return li;}));
 const cfg=data.settings;$('applied').textContent=`当前显示：${cfg.fixedBin<0?'自动跟踪':'固定中心 bin '+cfg.fixedBin} · DC ${cfg.dc?'开':'关'} · ${cfg.low}–${cfg.high} Hz · RX0 / 邻域 ±1。修改上方参数后需重新加载。`;
 $('details').textContent=JSON.stringify({settings:cfg,summary:s,markers:data.markers,provenance:data.provenance,cacheDirectory:data.cacheDirectory},null,2);
 cursor=extent()[0];$('scrub').min=extent()[0];$('scrub').max=extent()[1];$('scrub').value=cursor;
 makePlot('radarPlot','radar',[{label:'雷达位移估计',stroke:colors.radar}],visibleSeries(data.radar),'mm');
 makePlot('beltPlot','belt',[{label:'呼吸带原始值',stroke:colors.belt}],visibleSeries(data.belt),'ADC');
 makePlot('comparePlot','compare',[{label:'雷达频带',stroke:colors.radar},{label:'呼吸带频带',stroke:colors.belt}],visibleSeries(data.comparison),'SD');
 // uPlot commits constructor scales asynchronously; link only after its initial paint.
 requestAnimationFrame(()=>{for(const p of plots)p.setScale('x',{min:extent()[0],max:extent()[1]});requestAnimationFrame(()=>{linking=false;updateCursor();});});
}
function updateCursor(){
 $('scrub').value=cursor;$('time').textContent=cursor.toFixed(2)+' s';
 const width=Number($('window').value),[lo,hi]=extent();
 if(width){const left=Math.max(lo,Math.min(cursor-width/2,hi-width));linking=true;for(const p of plots)p.setScale('x',{min:left,max:Math.min(hi,left+width)});linking=false;}
 for(const p of plots)p.redraw();
}
function tick(now){if(playing&&data){cursor+=Math.min(.25,(now-lastTime)/1000)*Number($('speed').value);if(cursor>=extent()[1]){cursor=extent()[1];pause();}updateCursor();}lastTime=now;requestAnimationFrame(tick);}
$('play').onclick=()=>{if(!data)return;if(playing)pause();else{if(cursor>=extent()[1])cursor=extent()[0];playing=true;$('play').textContent='暂停';lastTime=performance.now();}};
$('scrub').oninput=()=>{pause();cursor=Number($('scrub').value);updateCursor();};
$('reset').onclick=()=>{pause();$('window').value='0';linking=true;for(const p of plots)p.setScale('x',{min:extent()[0],max:extent()[1]});linking=false;};
$('window').onchange=()=>{if(data){if($('window').value==='0')$('reset').click();else updateCursor();}};
$('scope').onchange=()=>{if(data){$('window').value='0';render();}};
$('mode').onchange=()=>{$('binLabel').hidden=$('mode').value!=='fixed';};
async function refresh(){try{const previous=$('sessions').value;const rows=await api('/api/replay/sessions');$('sessions').replaceChildren(...rows.map(s=>new Option(`${s.id} · ${s.hasBelt?'双设备':'仅雷达'} · ${s.state}${s.bytes===0?' / 无雷达数据':''}`,s.id)));if(rows.some(s=>s.id===previous))$('sessions').value=previous;}catch(e){$('error').textContent=e.message;}}
$('refresh').onclick=refresh;
$('load').onclick=async()=>{
 if(busy)return;busy=true;pause();$('error').textContent='';$('state').textContent='正在读取与提取波形，请稍候…';
 document.querySelectorAll('.card:first-of-type input,.card:first-of-type select,#load,#refresh').forEach(el=>el.disabled=true);
 try{data=await api('/api/replay',{session:$('sessions').value,fixedBin:$('mode').value==='fixed'?Number($('bin').value):-1,dc:$('dc').value==='on',low:Number($('low').value),high:Number($('high').value)});render();$('state').textContent='已加载 · 可拖动缩放或播放';}
 catch(e){$('error').textContent=e.message;$('state').textContent='加载失败；下方若有图，仍为上次已加载会话。';}
 finally{busy=false;document.querySelectorAll('.card:first-of-type input,.card:first-of-type select,#load,#refresh').forEach(el=>el.disabled=false);}
};
function download(name,text,type){const url=URL.createObjectURL(new Blob([text],{type}));const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
$('csv').onclick=()=>{if(!data)return;const lines=['relative_time_s,radar_bandpass_z,belt_bandpass_z'];const values=visibleSeries(data.comparison);for(let i=0;i<values[0].length;i++)lines.push(values.map(a=>a[i]??'').join(','));download(`${data.session}-${data.cacheKey}-${$('scope').value}.csv`,lines.join('\n'),'text/csv');};
$('json').onclick=()=>{if(data)download(`${data.session}-${data.cacheKey}.json`,JSON.stringify({...data,viewScope:$('scope').value},null,2),'application/json');};
new ResizeObserver(()=>{for(const p of plots)p.setSize({width:Math.max(280,p.root.parentElement.clientWidth),height:260});}).observe(document.querySelector('main'));
refresh();requestAnimationFrame(tick);
