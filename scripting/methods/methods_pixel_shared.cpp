// scripting/methods/methods_pixel_shared.cpp — world.BlendPixel and
// world.FilterPixel.
//
// Moved VERBATIM out of scripting/methods/methods_miniwindows.cpp.  Both are
// pure colour arithmetic on a pair of longs: they never touch m_MiniWindows,
// a CMiniWindow, a DC or a surface.  They were only in the miniwindow TU
// because that is where the rest of the Window* family lives — and that TU is
// mushgui-only, so the headless build linked stubs that returned
// eNoSuchWindow for every call and NO test could reach the real bodies at all.
//
// Nothing about the code changed; this is a build-graph move, so Windows
// (which compiles the whole family into one binary regardless) is unaffected.
// The file is in MUSHclient.vcxproj so the Windows build keeps compiling them.
//
// See also CMiniWindow::Filter / CMiniWindow::Blend, which apply the same
// operations across a whole surface using the same blending.h macros.

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../blending.h"
#include "../../color.h"
#include "../errors.h"

// see also: CMiniWindow::Filter

long CMUSHclientDoc::FilterPixel(long Pixel, short Operation, double Options) 
{
long r = GetRValue (Pixel),
     g = GetGValue (Pixel),
     b = GetBValue (Pixel);

   switch (Operation)
     {
     case 1:        // Noise
       {
       double threshold = Options / 100.0;
       r += (128 - genrand () * 256) * threshold;
       g += (128 - genrand () * 256) * threshold;
       b += (128 - genrand () * 256) * threshold;
       break;
       }

     case 2:    // MonoNoise
       {
       double threshold = Options / 100.0;
       long j = (128 - genrand () * 256) * threshold;
       r += j;
       g += j;
       b += j;
       break;
       }


     case  7: // Brightness     
        {
        r += Options;
        g += Options;
        b += Options;
        break;
        }


     case  8: // Contrast        
        {
        double c;

        c = r - 128;   // center on zero
        c *= Options;  // multiply by contrast
        r = c + 128;   // put back

        c = g - 128;   // center on zero
        c *= Options;  // multiply by contrast
        g = c + 128;   // put back

        c = b - 128;   // center on zero
        c *= Options;  // multiply by contrast
        b = c + 128;   // put back

        break;
        }


     case  9: // Gamma         
        {
        double c;

        if (Options < 0.0)
          Options = 0.0;

        c = ( (double) r) / 255.0;  // normalize it
        c = pow (c, Options);
        r = c * 255.0;

        c = ( (double) g) / 255.0;  // normalize it
        c = pow (c, Options);
        g = c * 255.0;

        c = ( (double) b) / 255.0;  // normalize it
        c = pow (c, Options);
        b = c * 255.0;

        break;
        }

     case  10: // ColourBrightness - red     
        {
        r += Options;
        break;
        }

     case  11: // ColourContrast - red        
        {
        double c;

        c = r - 128;   // center on zero
        c *= Options;  // multiply by contrast
        r = c + 128;   // put back

        break;
        }

     case  12: // ColourGamma - red         
        {
        double c;

        if (Options < 0.0)
          Options = 0.0;

        c = ( (double) r) / 255.0;  // normalize it
        c = pow (c, Options);
        r = c * 255.0;

        break;
        }

     case  13: // ColourBrightness - green     
        {
        g += Options;
        break;
        }

     case  14: // ColourContrast - green        
        {
        double c;

        c = g - 128;   // center on zero
        c *= Options;  // multiply by contrast
        g = c + 128;   // put back

        break;
        }

     case  15: // ColourGamma - green         
        {
        double c;

        if (Options < 0.0)
          Options = 0.0;

        c = ( (double) g) / 255.0;  // normalize it
        c = pow (c, Options);
        g = c * 255.0;

        break;
        }

     case  16: // ColourBrightness - blue     
        {
        b += Options;
        break;
        }

     case  17: // ColourContrast - blue        
        {
        double c;

        c = b - 128;   // center on zero
        c *= Options;  // multiply by contrast
        b = c + 128;   // put back

        break;
        }

     case  18: // ColourGamma - blue         
        {
        double c;

        if (Options < 0.0)
          Options = 0.0;

        c = ( (double) b) / 255.0;  // normalize it
        c = pow (c, Options);
        b = c * 255.0;

        break;
        }

     case 19: // MakeGreyscale  - linear
       {
        double c;
        c =  r + g + b;
        c /= 3;
        r = c;
        g = c;
        b = c;
        break;
       }

     case 20: // MakeGreyscale  - perceptual
       {
        double c;
        c = b * 0.11 + // blue   (perceptual)
            g * 0.59 + // green
            r * 0.30;  // red
        c /= 3;
        r = c;
        g = c;
        b = c;
        break;
       }

     case  21: // Brightness - multiplicative    
        {
        r *= Options;
        g *= Options;
        b *= Options;
        break;
        }

     case  22: // Brightness - multiplicative - red    
        {
        r *= Options;
        break;
        }

     case  23: // Brightness - multiplicative - green    
        {
        g *= Options;
        break;
        }

     case  24: // Brightness - multiplicative - blue    
        {
        b *= Options;
        break;
        }


     case 27:  // Average (of 1 pixel is itself)
       break;

     default: return -1;

     } // end of switch


	return RGB (CLAMP (r), CLAMP (g), CLAMP (b));
}   // end of CMUSHclientDoc::FilterPixel

#define Blend_It(Op) \
   do \
      if (Opacity < 1.0) \
        {  \
          r =  Blend_Opacity (rA, rB, Op, Opacity);\
          g =  Blend_Opacity (gA, gB, Op, Opacity);\
          b =  Blend_Opacity (bA, bB, Op, Opacity);\
        }  \
      else\
        {  \
          r = Op (rA, rB);\
          g = Op (gA, gB);\
          b = Op (bA, bB);\
        }  \
   while (false)


#define Colour_Op(fR,fG,fB) \
   do \
     {  \
      if (Opacity < 1.0) \
          {  \
          r = Simple_Opacity (rB, fR, Opacity); \
          g = Simple_Opacity (gB, fG, Opacity); \
          b = Simple_Opacity (bB, fB, Opacity); \
          }\
      else\
          {\
          r = fR;  \
          g = fG;  \
          b = fB;  \
          }\
       }\
   while (false)


// see also: CMiniWindow::BlendImage

long CMUSHclientDoc::BlendPixel(long Blend, long Base, short Mode, double Opacity) 
{
long rA = GetRValue (Blend),
     gA = GetGValue (Blend),
     bA = GetBValue (Blend);
long rB = GetRValue (Base),
     gB = GetGValue (Base),
     bB = GetBValue (Base);

long r, g, b;

  if (Opacity < 0.0 || Opacity > 1.0)
    return -2;

  // precompute cos table
  static unsigned char cos_table [256];
  static bool bTableComputed = false;

  if (!bTableComputed)
    {
    double pi_div255 = 3.1415926535898 / 255.0;
    long i;
    for (i = 0; i < 256; i++)
      {
      double a = 64.0 - cos ((double) i * pi_div255) * 64.0;
      cos_table [i] = (uint8) (a + 0.5);  // round
      }
    bTableComputed = true;
    } // table needs computing

  switch (Mode)
    {

    // normal modes
    case  1: Blend_It (Blend_Normal);     break;
    case  2: Blend_It (Blend_Average);    break;
    case  3: Blend_It (Blend_Interpolate);  break;

    case  4:  // dissolve - randomly choose pixels based on opacity
      {
       double rnd = genrand ();

        r = (rnd < Opacity) ? rA : rB;
        g = (rnd < Opacity) ? gA : gB;
        b = (rnd < Opacity) ? bA : bB;
      }
      break;


    // darkening modes
    case  5: Blend_It (Blend_Darken);     break;
    case  6: Blend_It (Blend_Multiply);   break;
    case  7: Blend_It (Blend_ColorBurn);  break;
    case  8: Blend_It (Blend_LinearBurn); break;
    case  9: Blend_It (Blend_InverseColorBurn);  break;
    case 10: Blend_It (Blend_Subtract);   break;
    
    // lightening modes
    case 11: Blend_It (Blend_Lighten);    break;
    case 12: Blend_It (Blend_Screen);     break;
    case 13: Blend_It (Blend_ColorDodge); break;
    case 14: Blend_It (Blend_LinearDodge);break;
    case 15: Blend_It (Blend_InverseColorDodge);  break;
    case 16: Blend_It (Blend_Add);        break;
      
    // soft/hard light etc.
    case 17: Blend_It (Blend_Overlay);    break;
    case 18: Blend_It (Blend_SoftLight);  break;
    case 19: Blend_It (Blend_HardLight);  break;
    case 20: Blend_It (Blend_VividLight); break;
    case 21: Blend_It (Blend_LinearLight);break;
    case 22: Blend_It (Blend_PinLight);   break;
    case 23: Blend_It (Blend_HardMix);    break;

    // difference modes
    case 24: Blend_It (Blend_Difference); break;
    case 25: Blend_It (Blend_Exclusion);  break;

    // glow modes
    case 26: Blend_It (Blend_Reflect);    break;
    case 27: Blend_It (Blend_Glow);       break;
    case 28: Blend_It (Blend_Freeze);     break;
    case 29: Blend_It (Blend_Heat);       break;
        
    case 30: Blend_It (Blend_Negation);   break;
    case 31: Blend_It (Blend_Phoenix);    break;
    case 32: Blend_It (Blend_Stamp);      break;

    // logical modes
    case 33: Blend_It (Blend_Xor);      break;
    case 34: Blend_It (Blend_And);      break;
    case 35: Blend_It (Blend_Or);       break;

    // the follow modes take one colour from the blend and retain 2 from the base
    case 36: Colour_Op (rA, gB, bB); break;  // red  
    case 37: Colour_Op (rB, gA, bB); break;  // green
    case 38: Colour_Op (rB, gB, bA); break;  // blue 

    // the follow modes take two colours from the blend and retain 1 from the base
    case 39: Colour_Op (rA, gA, bB); break;  // yellow 
    case 40: Colour_Op (rB, gA, bA); break;  // cyan    
    case 41: Colour_Op (rA, gB, bA); break;  // magenta    

    // limit green
    case 42: Colour_Op (rA, (gA > rA) ? rA : gA, bA); break;  // green limited by red    
    case 43: Colour_Op (rA, (gA > bA) ? bA : gA, bA); break;  // green limited by blue    
    case 44: Colour_Op (rA, (gA > ((rA + bA) / 2)) ? ((rA + bA) / 2) : gA, bA); break;  // green limited by average of red and blue    

    // limit blue
    case 45: Colour_Op (rA, gA, (bA > rA) ? rA : bA); break;  // blue limited by red    
    case 46: Colour_Op (rA, gA, (bA > gA) ? gA : bA); break;  // blue limited by green    
    case 47: Colour_Op (rA, gA, (bA > ((rA + gA) / 2)) ? ((rA + gA) / 2) : bA); break; // blue limited by average of red and green    

    // limit red
    case 48: Colour_Op ((rA > gA) ? gA : rA, gA, bA); break;  // red limited by green    
    case 49: Colour_Op ((rA > bA) ? bA : rA, gA, bA); break;  // red limited by blue    
    case 50: Colour_Op ((rA > ((gA + bA) / 2)) ? ((gA + bA) / 2) : rA, gA, bA); break;  // red limited by average of green and blue    

    // select single colour
    case 51: Colour_Op (rA, 0, 0); break;  // red only       (looks red)
    case 52: Colour_Op (0, gA, 0); break;  // green only     (looks green)
    case 53: Colour_Op (0, 0, bA); break;  // blue only      (looks blue)

    // discard single colour
    case 54: Colour_Op (0, gA, bA); break;  // discard red    (looks cyan)
    case 55: Colour_Op (rA, 0, bA); break;  // discard green  (looks magenta)
    case 56: Colour_Op (rA, gA, 0); break;  // discard blue   (looks yellow)

    // one colour to all channels  (ie. it looks grey)
    case 57: Colour_Op (rA, rA, rA); break;  // all red   
    case 58: Colour_Op (gA, gA, gA); break;  // all green   
    case 59: Colour_Op (bA, bA, bA); break;  // all blue   

    case 60:           // Hue mode
      {
          CColor cA, cB;    // A = blend, B = base

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetHue (cA.GetHue ());  // hue of blend image, others from base image
          r = Simple_Opacity (rB, cB.GetRed (), Opacity); 
          g = Simple_Opacity (gB, cB.GetGreen (), Opacity); 
          b = Simple_Opacity (bB, cB.GetBlue (), Opacity); 
       }
      break;

    case 61:           // Saturation mode
        {
          CColor cA, cB;    // A = blend, B = base

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetSaturation (cA.GetSaturation ());  // saturation of blend image, others from base image
          r = Simple_Opacity (rB, cB.GetRed (), Opacity); 
          g = Simple_Opacity (gB, cB.GetGreen (), Opacity); 
          b = Simple_Opacity (bB, cB.GetBlue (), Opacity); 
       }
      break;

    case 62:           // Colour mode
      {
          CColor cA, cB;    // A = blend, B = base

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetHue (cA.GetHue ());  // hue of blend image, 
          cB.SetSaturation (cA.GetSaturation ());  // saturation of blend image, luminance from base image
          r = Simple_Opacity (rB, cB.GetRed (), Opacity); 
          g = Simple_Opacity (gB, cB.GetGreen (), Opacity); 
          b = Simple_Opacity (bB, cB.GetBlue (), Opacity); 
       }
      break;


    case 63:           // Luminance mode
      {
          CColor cA, cB;    // A = blend, B = base

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetLuminance (cA.GetLuminance ());  // luminance from blend image, others from base image

          r = Simple_Opacity (rB, cB.GetRed (), Opacity); 
          g = Simple_Opacity (gB, cB.GetGreen (), Opacity); 
          b = Simple_Opacity (bB, cB.GetBlue (), Opacity); 

       }
      break;

    case 64:           // HSL (hue to red, saturation to green, luminance to blue)
        {
          CColor cA, cB;    // A = blend, B = base
          cA.SetRGB (rA, gA, bA);

          r = Simple_Opacity (rB, cB.GetRed (), Opacity); 
          g = Simple_Opacity (gB, cB.GetGreen (), Opacity); 
          b = Simple_Opacity (bB, cB.GetBlue (), Opacity); 

         }
      break;

     default: return -1;

     } // end of switch


	return RGB (CLAMP (r), CLAMP (g), CLAMP (b));
} // end of CMUSHclientDoc::BlendPixel
