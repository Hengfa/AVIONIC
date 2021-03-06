function [g] = gstar_tgv2_dmri(x,y,z,mri_obj,ts,t1,ld)


	%F(Kx)
	g1 = abs( backward_opt( x(:,:,:,1),mri_obj.mask,mri_obj.b1) - mri_obj.data);
	g1 = (ld/2)*sum(g1(:).^2);
	
	%F*(z)
	g2 = sum( mri_obj.data(:)'*z(:) ) + (1/(2*ld))*sum(abs(z(:)).^2);
	
	%G*(-Kx) (not for the real gap)
	
	g3 = -(1/ts)*bdiv_3_3( y(:,:,:,1:3) , t1/ts ) + forward_opt(z,mri_obj.mask,mri_obj.b1);
	g3 = sum(abs(g3(:)));
	
	g4 = -y(:,:,:,1:3) - (1/ts)*fdiv_3_6( y(:,:,:,4:9) , t1/ts ) ;
	g4 = sum(abs(g4(:)));
	
	g =   g1 + g2 + g3 + g4;
	
