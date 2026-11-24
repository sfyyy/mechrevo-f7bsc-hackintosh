(()=>{const deep=(n)=>{let t='';if(n.nodeType===3)return n.textContent;if(n.shadowRoot)t+=deep(n.shadowRoot);for(const c of n.childNodes)t+=deep(c);
 if(n.nodeType===1&&/^(TR|DIV|H3|H2|LI|P)$/.test(n.tagName))t+='\n';if(n.nodeType===1&&n.tagName==='TD')t+='\t';return t;};
return deep(document.body).replace(/\n\s*\n+/g,'\n')})()
