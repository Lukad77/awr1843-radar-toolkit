// Shared by browser and Node regression tests; formal intervals are half-open.
(function(root){
 function formalRange(data){return data.markers.formalWindow?.length===2 ? data.markers.formalWindow : null;}
 function extent(data,scope){
  if(scope==='formal'&&formalRange(data))return formalRange(data);
  const series=[data.radar,data.belt,data.comparison].filter(s=>s[0].length);
  return [Math.min(...series.map(s=>s[0][0])),Math.max(...series.map(s=>s[0].at(-1)))];
 }
 function series(data,values,scope){
  const range=scope==='formal'?formalRange(data):null;
  if(!range)return values;
  const result=values.map(()=>[]);
  values[0].forEach((t,i)=>{if(t>=range[0]&&t<range[1])values.forEach((row,k)=>result[k].push(row[i]));});
  return result;
 }
 function clockLabel(data){return data.timeBasis==='common_formal_t0'
  ? '时间零点为共同正式起点 T0；负时间为预采集。正式区间采用 [T0, T1)，不包含 T1 时刻。'
  : '旧会话未记录共同正式区间；时间零点为首个雷达完整帧接收时刻，仅提供全程视图。';}
 const api={formalRange,extent,series,clockLabel};
 if(typeof module!=='undefined'&&module.exports)module.exports=api;else root.ReplayScope=api;
})(globalThis);
