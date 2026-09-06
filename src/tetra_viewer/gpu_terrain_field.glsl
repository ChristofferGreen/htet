// Shared P7 terrain field helpers. The including shader provides lane(i,c).
bool terrain_finite(float v){return !isnan(v)&&!isinf(v);}
float terrain_saturate(float v){return clamp(v,0.0,1.0);}
float terrain_smooth01(float v){v=terrain_saturate(v);return v*v*(3.0-2.0*v);}
vec3 terrain_gradient(uint h){const float s=.7071067811865475;switch(h%12u){case 0u:return vec3(s,s,0);case 1u:return vec3(-s,s,0);case 2u:return vec3(s,-s,0);case 3u:return vec3(-s,-s,0);case 4u:return vec3(s,0,s);case 5u:return vec3(-s,0,s);case 6u:return vec3(s,0,-s);case 7u:return vec3(-s,0,-s);case 8u:return vec3(0,s,s);case 9u:return vec3(0,-s,s);case 10u:return vec3(0,s,-s);default:return vec3(0,-s,-s);}}
uint terrain_hash3(ivec3 v){uint h=uint(v.x)*0x8da6b343u^uint(v.y)*0xd8163841u^uint(v.z)*0xcb1ab31fu;h^=h>>13u;h*=0x85ebca6bu;h^=h>>16u;return h;}
float terrain_fade(float v){return v*v*v*(v*(v*6.0-15.0)+10.0);}
float terrain_noise(vec3 p){ivec3 c=ivec3(floor(p));vec3 f=fract(p);float q[8];uint n=0u;for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x){ivec3 d=ivec3(x,y,z);q[n++]=dot(terrain_gradient(terrain_hash3(c+d)),f-vec3(d));}float u=terrain_fade(f.x),v=terrain_fade(f.y),w=terrain_fade(f.z);return mix(mix(mix(q[0],q[1],u),mix(q[2],q[3],u),v),mix(mix(q[4],q[5],u),mix(q[6],q[7],u),v),w);}
float terrain_resolved(float f,float p){return (!(p>0.0)||!(abs(f)>0.0))?1.0:terrain_smooth01(1.0/(abs(f)*p)-1.75);}
// This is the complete P7a tuple grammar.  `lane` is intentionally supplied
// by each consuming shader so the field ABI remains one shared definition.
float terrain_field_displacement(vec3 direction,float footprint){
  const float secondary=lane(1u,1u),frequency=lane(1u,2u);
  const vec4 a=vec4(lane(2u,0u),lane(2u,1u),lane(2u,2u),lane(2u,3u));
  const vec4 b=vec4(lane(3u,0u),lane(3u,1u),lane(3u,2u),lane(3u,3u));
  const vec4 c=vec4(lane(4u,0u),lane(4u,1u),lane(4u,2u),lane(4u,3u));
  const vec4 d=vec4(lane(5u,0u),lane(5u,1u),lane(5u,2u),lane(5u,3u));
  const vec4 e=vec4(lane(6u,0u),lane(6u,1u),lane(6u,2u),lane(6u,3u));
  const vec4 f=vec4(lane(7u,0u),lane(7u,1u),lane(7u,2u),lane(7u,3u));
  const float radius=f.z;
  float height=a.x;
  #define TERRAIN_SAMPLE(freq,offset) terrain_noise(direction*(radius*(freq))+(offset))
  height+=a.y*terrain_resolved(a.z,footprint)*TERRAIN_SAMPLE(a.z,vec3(11.371,-7.219,3.117));
  const float mountain_scale=b.w;
  const float range=terrain_saturate((TERRAIN_SAMPLE(a.w*0.0+b.y*mountain_scale,vec3(-20.077,30.861,7.193))-.08)/.30);
  const float ridge=terrain_saturate((1.0-abs(TERRAIN_SAMPLE(b.x*mountain_scale,vec3(6.691,13.733,-4.337)))-.30)/.68);
  const float range_resolution=terrain_resolved(b.y*mountain_scale,footprint);
  const float ridge_resolution=terrain_resolved(b.x*mountain_scale,footprint);
  const float carrier=mix(.6634,ridge*ridge,ridge_resolution);
  float mountain_fade=1.0;
  if(c.y>c.x)mountain_fade=terrain_smooth01((acos(clamp(direction.y,-1.0,1.0))*radius-c.x)/(c.y-c.x));
  height+=a.w*b.z*mountain_fade*range_resolution*range*range*carrier;
  height+=c.z*terrain_resolved(c.w,footprint)*TERRAIN_SAMPLE(c.w,vec3(4.117,7.731,-2.513));
  const float region=terrain_saturate((TERRAIN_SAMPLE(d.z,vec3(-2.719,5.303,8.117))+.12)/.30);
  height+=d.x*min(terrain_resolved(d.z,footprint),terrain_resolved(d.y,footprint))*region*TERRAIN_SAMPLE(d.y,vec3(-6.337,1.819,9.173));
  height+=e.z*terrain_resolved(e.w,footprint)*TERRAIN_SAMPLE(e.w,vec3(13.117,-11.731,2.337));
  float amplitude=secondary,current_frequency=frequency;
  for(int octave=0;octave<4;++octave){height+=amplitude*terrain_resolved(current_frequency,footprint)*TERRAIN_SAMPLE(current_frequency,vec3(0));amplitude*=.25;current_frequency*=2.0;}
  if(f.y>f.x)height*=terrain_smooth01((acos(clamp(direction.y,-1.0,1.0))*radius-f.x)/(f.y-f.x));
  #undef TERRAIN_SAMPLE
  return height;
}
float terrain_field_signed_distance_world(vec3 world,float footprint){
  const vec3 centre=vec3(lane(0u,0u),lane(0u,1u),lane(0u,2u));
  const float planet_radius=lane(7u,2u);const vec3 offset=world-(centre-vec3(0,planet_radius,0));
  const float length_offset=length(offset);if(!(length_offset>1.e-15))return -planet_radius;
  float displacement=terrain_field_displacement(offset/length_offset,footprint);
  if(lane(7u,3u)==1.0){const float half_width=max(lane(8u,2u),1.e-12);displacement=lane(8u,1u)*max(0.0,1.0-abs((world.z-centre.z)-lane(8u,0u))/half_width);}
  return length_offset-(planet_radius+displacement);
}
float terrain_field_signed_distance(vec3 root_point,float footprint){
  return terrain_field_signed_distance_world(root_point*lane(9u,3u)+vec3(lane(9u,0u),lane(9u,1u),lane(9u,2u)),footprint);
}
vec3 terrain_field_normal_world(vec3 point,float footprint){
  const float epsilon=1.e-5;
  const vec3 gradient=vec3(terrain_field_signed_distance_world(point+vec3(epsilon,0,0),footprint)-terrain_field_signed_distance_world(point-vec3(epsilon,0,0),footprint),terrain_field_signed_distance_world(point+vec3(0,epsilon,0),footprint)-terrain_field_signed_distance_world(point-vec3(0,epsilon,0),footprint),terrain_field_signed_distance_world(point+vec3(0,0,epsilon),footprint)-terrain_field_signed_distance_world(point-vec3(0,0,epsilon),footprint));
  const float length_gradient=length(gradient);return length_gradient>1.e-15?gradient/length_gradient:vec3(0,1,0);
}
vec3 terrain_field_project_to_surface_world(vec3 point,float footprint){
  for(int iteration=0;iteration<12;++iteration){const float distance=terrain_field_signed_distance_world(point,footprint);if(abs(distance)<1.e-10)break;point-=terrain_field_normal_world(point,footprint)*distance;}
  return point;
}
