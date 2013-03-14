queueop.cmo: 
queueop.cmx: 
xsraw.cmo: queueop.cmo xsraw.cmi 
xsraw.cmx: queueop.cmx xsraw.cmi 
xst.cmo: xsraw.cmi xst.cmi 
xst.cmx: xsraw.cmx xst.cmi 
xs.cmo: xst.cmi xsraw.cmi xs.cmi 
xs.cmx: xst.cmx xsraw.cmx xs.cmi 
xs.cmi: xst.cmi xsraw.cmi 
xsraw.cmi: 
xst.cmi: xsraw.cmi 
