from pathlib import Path
import subprocess,sys
source=Path(__file__).parent
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
compiler=sys.argv[2]
for line in (source/'formats.inc').read_text().splitlines():
 name,qualifier,kind,channels,extended=[part.strip().strip(chr(34)).strip(chr(39)) for part in line[7:-1].split(',')]
 prefix={'f':'','u':'u','i':'i'}[kind]
 vector=prefix+'vec4'
 for mode in ['sample','read','unformatted','unformatted_modify','write','modify']:
  body='#version 450\n#extension GL_EXT_shader_image_load_formatted : require\nlayout(local_size_x=8,local_size_y=8) in;\n'
  if mode=='sample':
   body+=f'layout(set=0,binding=2) uniform {prefix}sampler2D tex;\n'
  else:
   layout='' if mode.startswith('unformatted') else ','+qualifier
   access='readonly ' if mode in ['read','unformatted'] else ('writeonly ' if mode=='write' else '')
   body+=f'layout(set=0,binding=0{layout}) {access}uniform {prefix}image2D img;\n'
  body+='layout(std430,set=0,binding=1) buffer Result { vec4 values[]; };\nvoid main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy); if(any(greaterThanEqual(p,ivec2(16))))return; uint index=uint(p.y*16+p.x);\n'
  if mode=='sample':
   body+='values[index]=vec4(texelFetch(tex,p,0));'
  elif mode in ['read','unformatted']:body+='values[index]=vec4(imageLoad(img,p));'
  elif mode=='write':body+=f'imageStore(img,p,{vector}((p.x^p.y)&1,p.x&1,p.y&1,1));'
  elif mode=='unformatted_modify':body+=f'{vector} v=imageLoad(img,p);{vector} pattern={vector}((p.x^p.y)&1,p.x&1,p.y&1,1);imageStore(img,p,mix({vector}(1)-pattern,pattern,notEqual(v,{vector}(0))));memoryBarrierImage();values[index+256]=vec4(imageLoad(img,p));'
  else:body+=f'{vector} v=imageLoad(img,p);imageStore(img,p,{vector}(1)-v);memoryBarrierImage();values[index]=vec4(imageLoad(img,p));'
  exact={'R8G8_UINT':'165u','R8G8_SINT':'-91','R16G16_UINT':'42330u','R16G16_SINT':'-23206','A2B10G10R10_UINT_PACK32':'677u','R32G32_UINT':'0x80000011u','R32G32_SINT':'int(0x80000011u)'}
  if mode=='unformatted' and name in exact:
   body+='if(imageLoad(img,p).x!='+exact[name]+')values[index]=vec4(-12345);'
  body+='}\n'
  shader=out/(name+'_'+mode+'.comp');shader.write_text(body)
  subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader.with_suffix('.spv'))],check=True,stdout=subprocess.DEVNULL)

for stage,body in {
 'vert': 'layout(location=0) flat out uint valid;void main(){vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));int i=gl_VertexIndex-int(data.base_vertex);valid=(i>=0&&i<3&&gl_InstanceIndex==int(data.base_instance))?1u:0u;gl_Position=vec4(positions[clamp(i,0,2)],data.depth,1);}',
 'frag': 'layout(location=0) flat in uint valid;layout(location=0) out vec4 color;void main(){color=valid==1u?data.color:vec4(1,1,0,1);}'
}.items():
 shader=out/('graphics.'+stage)
 shader.write_text('#version 450\nlayout(push_constant) uniform Push {vec4 color;float depth;uint base_vertex;uint base_instance;uint padding;} data;\n'+body)
 subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader)+'.spv'],check=True)

vertex=out/'graphics.vert'
body=vertex.read_text().replace('gl_Position=vec4(positions[clamp(i,0,2)],data.depth,1);','if(data.padding==1u)positions[clamp(i,0,2)]=vec2(0);if(data.padding==2u)positions[clamp(i,0,2)]=vec2(i==0?-0.75:0.75,0);gl_PointSize=1;gl_Position=vec4(positions[clamp(i,0,2)],data.depth,1);')
vertex.write_text(body)
subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(vertex),'-o',str(vertex)+'.spv'],check=True)
for stage,member in [('vert','vertex_count'),('frag','fragment_count')]:
 text=(out/('graphics.'+stage)).read_text()
 text=text.replace('void main(){','layout(std430,set=0,binding=0) buffer Counters { uint vertex_count;uint fragment_count; } counters;void main(){atomicAdd(counters.'+member+',1u);')
 shader=out/('effects.'+stage);shader.write_text(text)
 subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader)+'.spv'],check=True)


for name in ['R8_SNORM','R8G8B8A8_SNORM','R16_UNORM','R16G16B16A16_UNORM','R8_UNORM','R8G8B8A8_UNORM','R16_SFLOAT','R16G16B16A16_SFLOAT','R32_SFLOAT','R32G32B32A32_SFLOAT','R8G8_UNORM','R8G8_SNORM','R16G16_SFLOAT','R32G32_SFLOAT','A2B10G10R10_UNORM_PACK32','B10G11R11_UFLOAT_PACK32','R16_SNORM','R8G8_UINT','R8G8_SINT','R16G16_UNORM','R16G16_SNORM','R16G16_UINT','R16G16_SINT','A2B10G10R10_UINT_PACK32','R32G32_UINT','R32G32_SINT','R16G16B16A16_SNORM']:
 for mode in ['unformatted','unformatted_modify']:
  body=(out/(name+'_'+mode+'.comp')).read_text()
  body=body.replace('image2D img;', 'image2D img[2];').replace('imageLoad(img,p)', 'imageLoad(img[1],p)').replace('imageStore(img,p,', 'imageStore(img[0],p,')
  shader=out/(name+'_'+mode+'_array.comp');shader.write_text(body)
  subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader.with_suffix('.spv'))],check=True,stdout=subprocess.DEVNULL)

dxc=sys.argv[3]
for name,channels,bits,signed in [('R8_SNORM',1,8,True),('R8G8B8A8_SNORM',4,8,True),('R16_UNORM',1,16,False),('R16G16B16A16_UNORM',4,16,False)]:
 uint_type='uint' if channels==1 else 'uint4'
 float_type='float' if channels==1 else 'float4'
 decode=(f'max(float4((int4(raw)<<{32-bits})>>{32-bits})/{(1<<(bits-1))-1}.0,-1.0)' if signed else f'float4(raw)/{(1<<bits)-1}.0')
 if channels==1:
  decode=(f'max(float((int(raw)<<{32-bits})>>{32-bits})/{(1<<(bits-1))-1}.0,-1.0)' if signed else f'float(raw)/{(1<<bits)-1}.0')
 body=f'''globallycoherent RWTexture2D<{uint_type}> source:register(u0);
globallycoherent RWTexture2D<{float_type}> target:register(u1);
RWStructuredBuffer<float4> result:register(u2);
cbuffer Phase:register(b0){{uint phase;}};
float4 pattern(uint2 p){{return float4((p.x%5)*0.25,(p.y%5)*0.25,((p.x+p.y)%5)*0.25,((p.x^p.y)%5)*0.25);}}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){{
 uint2 p=id.xy;uint index=p.y*16+p.x;
 float4 v=pattern(p);
 if({'true' if signed else 'false'})v=v*2-1;
 if(phase==0){{target[p]=v{'.x' if channels==1 else ''};return;}}
 {uint_type} raw=source[p];
 {float_type} decoded={decode};
 result[index+(phase==2?256:0)]=float4({'decoded,0,0,1' if channels==1 else 'decoded'});
 if(phase==1){{target[p]={'(-v)' if signed else '(1-v)'}{'.x' if channels==1 else ''};}}
}}
'''
 shader=out/(name+'_alias.hlsl');shader.write_text(body)
 subprocess.run([dxc,'-T','cs_6_4','-E','main','-O3',str(shader),'-Fo',str(shader.with_suffix('.dxil'))],check=True)


for code,(name,size) in enumerate([('R8G8_UNORM',2),('R8G8_SNORM',2),('R16G16_SFLOAT',4),('R32G32_SFLOAT',8),('A2B10G10R10_UNORM_PACK32',4),('B10G11R11_UFLOAT_PACK32',4)]):
 subprocess.run([dxc,'-T','cs_6_4','-E','main','-O3','-D',f'RAW_FORMAT={code}','-D',f'RAW_BYTES={size}',str(source/'raw_formats.hlsl'),'-Fo',str(out/(name+'_raw.dxil'))],check=True)


for code,(name,size) in enumerate([('R8G8_UNORM',2),('R8G8_SNORM',2),('R16G16_SFLOAT',4),('R32G32_SFLOAT',8),('A2B10G10R10_UNORM_PACK32',4),('B10G11R11_UFLOAT_PACK32',4)]):
 subprocess.run([compiler,'-V','--target-env','vulkan1.2',f'-DRAW_FORMAT={code}',f'-DRAW_BYTES={size}',str(source/'raw_bridge.comp'),'-o',str(out/(name+'_persistent.spv'))],check=True)
 text='#version 450\nlayout(local_size_x=8,local_size_y=8) in;layout(set=0,binding=2) uniform sampler2D tex;layout(std430,set=0,binding=1) buffer Result {vec4 values[];} result;layout(push_constant) uniform Parameters {uint row_pitch;uint phase;uint offset;uint base;uint width;uint height;} data;void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=int(data.width)||p.y>=int(data.height))return;result.values[data.base+512+p.y*16+p.x]=texelFetch(tex,p,0);}'
 shader=out/(name+'_persistent_sample.comp');shader.write_text(text)
 subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader.with_suffix('.spv'))],check=True)

for code,name in enumerate(['R8G8_UNORM','R8G8_SNORM','R16G16_SFLOAT','R32G32_SFLOAT','A2B10G10R10_UNORM_PACK32','B10G11R11_UFLOAT_PACK32']):
 subprocess.run([compiler,'-V','--target-env','vulkan1.2',f'-DRAW_FORMAT={code}',str(source/'automatic_image.comp'),'-o',str(out/(name+'_automatic.spv'))],check=True)

for mode in ['storage_attachment','sampler_attachment','storage_sampler_attachment','separate_storage_sampler_attachment','immutable_storage_sampler_attachment']:
 body='#version 450\n#extension GL_EXT_shader_image_load_formatted : require\nlayout(location=0) out vec4 color;layout(set=0,binding=0) coherent uniform image2D img;layout(std430,set=0,binding=1) buffer Counters{uint count;uint unused;} counters;layout(set=0,binding=2) uniform sampler2D tex;void main(){ivec2 p=ivec2(gl_FragCoord.xy);ivec2 target=p+ivec2(7,0);atomicAdd(counters.count,1u);color=vec4(.125,.25,.5,1);'
 if mode != 'sampler_attachment':body+='imageStore(img,target,vec4(.75,.5,.25,1));memoryBarrierImage();'
 if mode != 'storage_attachment':body+='color=textureLod(tex,(vec2(target)+.5)/16.,0);'
 if mode.startswith('separate_'):
  body=body.replace('layout(set=0,binding=2) uniform sampler2D tex;', 'layout(set=0,binding=2) uniform texture2D image;layout(set=0,binding=3) uniform sampler state;').replace('textureLod(tex,','textureLod(sampler2D(image,state),')
 body+='}'
 shader=out/('simultaneous_'+mode+'.frag');shader.write_text(body)
 subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader)+'.spv'],check=True)

for name in ['R8G8_UNORM','R8G8_SNORM','R16G16_SFLOAT','R32G32_SFLOAT','A2B10G10R10_UNORM_PACK32','B10G11R11_UFLOAT_PACK32']:
 for mode in ['1d','1d_array','3d','cube','cube_array']:
  one=mode.startswith('1d');cube=mode.startswith('cube');array=mode.endswith('array');volume=mode=='3d'
  layers=(12 if array else 6) if cube else (2 if array else 1)
  itype='1DArray' if one and array else '1D' if one else '2DArray' if cube else '3D'
  stype='CubeArray' if cube and array else 'Cube' if cube else itype
  store='ivec2(x,z)' if one and array else 'x' if one else 'ivec3(x,y,z)'
  coord='vec2((x+.5)/w,z)' if one and array else '(x+.5)/w' if one else 'vec3((vec3(x,y,z)+.5)/vec3(w,h,d))'
  gradient='0.' if one else 'vec3(0)'
  if cube:
   coord='vec4(direction((vec2(x,y)+.5)/vec2(w,h),z%6),z/6)' if array else 'direction((vec2(x,y)+.5)/vec2(w,h),z)'
  text='#version 450\n#extension GL_EXT_shader_image_load_formatted : require\nlayout(local_size_x=1) in;layout(set=0,binding=0) coherent uniform image'+itype+' img[2];layout(set=0,binding=2) uniform sampler'+stype+' tex;layout(std430,set=0,binding=1) buffer Result{vec4 values[];} result;'
  if cube:text+='vec3 direction(vec2 uv,int face){vec2 p=uv*2-1;switch(face){case 0:return vec3(1,-p.y,-p.x);case 1:return vec3(-1,-p.y,p.x);case 2:return vec3(p.x,1,p.y);case 3:return vec3(p.x,-1,-p.y);case 4:return vec3(p.x,-p.y,1);default:return vec3(-p.x,-p.y,-1);}}'
  text+='void main(){for(int mip=0;mip<2;mip++){int w=8>>mip;int h='+('1' if one else 'w')+';int d='+('4>>mip' if volume else str(layers))+';for(int z=0;z<d;z++)for(int y=0;y<h;y++)for(int x=0;x<w;x++){vec4 color=vec4('+('float((z+mip)%8)*.125,float((z*3+mip+1)%8)*.125' if cube else 'float((x+z+mip)%4)*.25,float((y+z+mip+1)%4)*.25')+',.5,1);if(mip==0)imageStore(img[0],'+store+',color);else imageStore(img[1],'+store+',color);}}memoryBarrierImage();int index=0;for(int mip=0;mip<2;mip++){int w=8>>mip;int h='+('1' if one else 'w')+';int d='+('4>>mip' if volume else str(layers))+';for(int z=0;z<d;z++)for(int y=0;y<h;y++)for(int x=0;x<w;x++,index++){result.values[index]=textureLod(tex,'+coord+',mip);if(mip==0)result.values[1024+index]=textureGrad(tex,'+coord+','+gradient+','+gradient+');else result.values[1024+index]=textureLod(tex,'+coord+',mip);'
  if not cube:text+='result.values[2048+index]=texelFetch(tex,'+store+',mip);'
  text+='}}'
  query='vec3(textureSize(tex,0),1,1)' if one and not array else 'vec3(textureSize(tex,0),1)' if one or cube and not array else 'vec3(textureSize(tex,0))'
  text+='result.values[4095]=vec4('+query+',textureQueryLevels(tex));'
  if cube:
   for index,direction,lod in [(4092,'vec3(1,0,1)','0.'),(4093,'vec3(1,1,1)','0.'),(4094,'vec3(1,0,0)','.5')]:
    ccoord='vec4('+direction+',0)' if array else direction
    text+=f'result.values[{index}]=textureLod(tex,{ccoord},{lod});'
  if cube:
   ccoord='vec4(vec3(1,0,1),0)' if array else 'vec3(1,0,1)'
   gcoord='vec4(vec3(1,0,0),0)' if array else 'vec3(1,0,0)'
   text+='result.values[4090]=textureGather(tex,'+ccoord+',0);result.values[4091]=textureGrad(tex,'+gcoord+',vec3(0,0,-.5),vec3(0,-.5,0));'
  else:
   layer='1' if array else '0'
   edge='vec2(1./8.,'+layer+')' if one and array else '1./8.' if one else 'vec3(1./8.,.5/8.,.5/4.)'
   spatial=edge if one else 'vec3(1./8.,1./8.,1./4.)'
   gradcoord='vec2(.5/4.,'+layer+')' if one and array else '.5/4.' if one else 'vec3(.5/4.,.5/4.,.5/2.)'
   mixcoord='vec2(.5/8.,'+layer+')' if one and array else '.5/8.' if one else 'vec3(.5/8.,.5/8.,.5/4.)'
   gx='.25' if one else 'vec3(.25,0,0)';gy='0.' if one else 'vec3(0,.25,0)'
   text+='result.values[4090]=textureLod(tex,'+edge+',0);result.values[4091]=textureLod(tex,'+spatial+',0);result.values[4092]=textureGrad(tex,'+gradcoord+','+gx+','+gy+');result.values[4093]=textureLod(tex,'+mixcoord+',.5);'
  text+='}'
  shader=out/(name+'_dimension_' +mode+'.comp');shader.write_text(text)
  subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader.with_suffix('.spv'))],check=True,stdout=subprocess.DEVNULL)

shader=out/'simultaneous_sampler_readonly_attachment.frag'
shader.write_text('#version 450\nlayout(location=0) out vec4 color;layout(set=0,binding=2) uniform sampler2D tex;void main(){vec2 p=gl_FragCoord.xy+vec2(7,0);vec4 sampled=texture(tex,p/16.);color=vec4(1-sampled.r,0,0,1);}')
subprocess.run([compiler,'-V','--target-env','vulkan1.2',str(shader),'-o',str(shader)+'.spv'],check=True)
