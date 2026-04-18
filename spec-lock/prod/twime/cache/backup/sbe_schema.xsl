<xsl:stylesheet version="1.0" 
    xmlns="http://www.w3.org/1999/xhtml" 
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform" 
    xmlns:sbe="http://fixprotocol.io/2016/sbe"
>
    
    <xsl:output encoding="utf-8" indent="yes" method="html" version="5.0"/>
    
    <xsl:variable name="schema_description">
        <xsl:variable name="package" select="/sbe:messageSchema/@package"/>
            <xsl:choose>
                <xsl:when test="$package='moex_spectra_simba'">MOEX SIMBA SPECTRA</xsl:when>
                <xsl:when test="$package='moex_spectra_twime'">MOEX TWIME SPECTRA</xsl:when>
                <xsl:when test="$package='moex_spectra_rfs_twime'">MOEX RFS TWIME</xsl:when>
                <xsl:when test="not($package)">{unspecified protocol package}</xsl:when>
                <xsl:otherwise><xsl:value-of select="$package"/></xsl:otherwise>
            </xsl:choose>
        <xsl:text> message schema</xsl:text>
    </xsl:variable>
    
    <xsl:template match="/sbe:messageSchema">
        <html>
            <head>
                <title><xsl:value-of select="$schema_description"/></title>
                <xsl:call-template name="t_css"/>
            </head>
            
            <body>
                <div class="schema-title"><xsl:value-of select="$schema_description"/></div>
                <div class="schema-version"><xsl:value-of select="concat('version = ', @version, ', id = ', @id)"/></div>
                <!-- generate table of content -->
                <xsl:call-template name="t_toc">
                    <xsl:with-param name="section_node" select="sbe:message"/>
                    <xsl:with-param name="section_name">Messages</xsl:with-param> 
                </xsl:call-template>
                <xsl:call-template name="t_toc">
                    <xsl:with-param name="section_node" select="/sbe:messageSchema/types/composite"/>
                    <xsl:with-param name="section_name">Composites</xsl:with-param> 
                </xsl:call-template>
                <xsl:call-template name="t_toc">
                    <xsl:with-param name="section_node" select="types/enum"/>
                    <xsl:with-param name="section_name">Enumerations</xsl:with-param> 
                </xsl:call-template>
                <xsl:call-template name="t_toc">
                    <xsl:with-param name="section_node" select="types/set"/>
                    <xsl:with-param name="section_name">Sets</xsl:with-param> 
                </xsl:call-template>
                <xsl:call-template name="t_toc">
                    <xsl:with-param name="section_node" select="types/type"/>
                    <xsl:with-param name="section_name">Types</xsl:with-param> 
                </xsl:call-template>
                <!-- generate content tables -->
                <xsl:call-template name="t_messages"/>
				<xsl:call-template name="t_composites"/>
                <xsl:call-template name="t_enums"/>
                <xsl:call-template name="t_sets"/>
                <xsl:call-template name="t_types"/>
                <div class="footer">(End of document)</div>
            </body>
        </html>
    </xsl:template>
    
    <xsl:template name="t_toc">
        <xsl:param name="section_node"/>
        <xsl:param name="section_name"/>
        <div class="toc_section">
            <a href="#{$section_name}"><xsl:value-of select="$section_name"/></a>
            <ul>
                <xsl:for-each select="$section_node">
                    <li>
                        <a href="#{@name}">
                            <span><xsl:value-of select="@name"/></span>
                            <span><xsl:if test="@id">[<xsl:value-of select="@id"/>]</xsl:if></span>
                        </a>
                    </li>
                </xsl:for-each>
            </ul>
        </div>
    </xsl:template>

    <xsl:template name="indent">
        <xsl:param name="i"/>
        <xsl:if test="$i > 0">
            <span class="x-indent"/>
            <xsl:call-template name="indent"><xsl:with-param name="i" select="$i - 1"/></xsl:call-template>
        </xsl:if>
    </xsl:template>
    
    <xsl:template name="field">
        <xsl:param name="i"/>
        <tr>
            <td>
                <xsl:call-template name="indent"><xsl:with-param name="i" select="$i"/></xsl:call-template>
                <span><xsl:value-of select="@name"/></span>
            </td>
            <td><xsl:value-of select="@id"/></td>
            <td><a href="#{concat(@dimensionType,@type)}"><xsl:value-of select="concat(@dimensionType,@type)"/></a></td>
        </tr>           
    </xsl:template>
    
    <xsl:template match="field|data">
        <xsl:param name="i" select="0"/>
        <xsl:call-template name="field">
            <xsl:with-param name="i" select="$i"/>
        </xsl:call-template>
    </xsl:template>
    
    <xsl:template match="group">
        <xsl:param name="i" select="0"/>
        <xsl:call-template name="field">
            <xsl:with-param name="i" select="$i"/>
        </xsl:call-template>
        <xsl:apply-templates>
            <xsl:with-param name="i" select="$i + 1"/>
        </xsl:apply-templates>
    </xsl:template>
    
    <xsl:template name="t_messages">
		<div id="Messages" class="entity-anchor"/>
        <div class="content messages">
            <span>Messages</span>
            <xsl:for-each select="/sbe:messageSchema/sbe:message">
				<div id="{@name}" class='entity-anchor'/>
				<table>
					<tr><th>message name</th><th>id</th><th>type</th></tr>
					<tr>
						<td class="entity-name"><span><xsl:value-of select="@name"/></span></td>
						<td><xsl:value-of select="@id"/></td>
						<td>message</td>
					</tr>
					<xsl:if test="*">
						<tr><th>field name</th><th>id</th><th>type</th></tr>
						<xsl:apply-templates/>
					</xsl:if>                   
				</table>
            </xsl:for-each>
        </div>
    </xsl:template>
	
	  
    <xsl:template name="t_composites">
		<div id="Composites" class="entity-anchor"/>
        <div class="content composites">
			<span>Composites</span>
            <xsl:for-each select="/sbe:messageSchema/types/composite">
				<div id="{@name}" class='entity-anchor'/>
                <table>
                    <tr><th>composite name</th><th colspan="6">description</th></tr>
                    <tr>
                        <td class="entity-name"><span><xsl:value-of select="@name"/></span></td>
                        <td colspan="6"><xsl:value-of select="@description"/></td>
                    </tr>
                    <xsl:call-template name="t_type_properties_headers"/>                                     
                    <xsl:for-each select="(.)/*">
                        <xsl:call-template name="t_type_properties"/>
                    </xsl:for-each>
                </table>
            </xsl:for-each>
        </div>
    </xsl:template>
    
    <xsl:template name="t_enums">
		<div id="Enumerations" class="entity-anchor"/>
		<div class="content enums">
			<span>Enumerations</span>
            <xsl:for-each select="/sbe:messageSchema/types/enum">
				<div id="{@name}" class='entity-anchor'/>
				<table>
					<tr><th>enum name</th><th>encoding type</th></tr>
					<tr>
						<td class="entity-name"><span><xsl:value-of select="@name"/></span></td>
						<td><a href="#{@encodingType}"><xsl:value-of select="@encodingType"/></a></td>
					</tr>
					<tr><th>name</th><th>value</th></tr>
					<xsl:for-each select="(.)/validValue">
					<tr>
						<td><xsl:value-of select="@name"/></td>
						<td><xsl:value-of select="text()"/></td>
					</tr>
					</xsl:for-each>
				</table>
            </xsl:for-each>
        </div>
    </xsl:template>
    
    <xsl:template name="t_sets">
        <div id="Sets" class="entity-anchor"/>
		<div class="content sets">
			<span>Sets</span>
            <xsl:for-each select="/sbe:messageSchema/types/set">
				<div id="{@name}" class='entity-anchor'/>
                <table>
                    <tr><th>set name</th><th>encoding type</th></tr>
                    <tr>
                        <td class="entity-name"><span><xsl:value-of select="@name"/></span></td>
                        <td><xsl:value-of select="@encodingType"/></td>
                    </tr>
                    <tr><th>choice name</th><th>value</th></tr>
                    <xsl:for-each select="(.)/choice">
                    <tr>
                        <td><xsl:value-of select="@name"/></td>
                        <td><xsl:value-of select="text()"/></td>
                    </tr>
                    </xsl:for-each>
                </table>
            </xsl:for-each>
        </div>
    </xsl:template>
  
    
    <xsl:template name="t_types">
		<div id="Types" class="entity-anchor"/>
        <div class="content types">
            <span class="content_header">Types</span>
            <table>
            <xsl:call-template name="t_type_properties_headers"/>
                <xsl:for-each select="/sbe:messageSchema/types/type">
                    <xsl:call-template name="t_type_properties"/>
                </xsl:for-each>
            </table>
        </div>
    </xsl:template>
    
    <xsl:template name="t_type_properties_headers">
        <tr>
            <th>name</th>
            <th>primitive type</th>
            <th>length</th>
            <th>presence</th>
            <th>min value</th>
            <th>max value</th>
            <th>null value</th>
        </tr>
    </xsl:template>
    <xsl:template name="t_type_properties">
		<tr id="{@name}">
		<xsl:choose>
			<xsl:when test="name()='ref'" >
				<td><xsl:value-of select="@name"/></td>
				<td><a href="#{@type}"><xsl:value-of select="@type"/></a></td>
				<td colspan="5"/>
			</xsl:when>
			<xsl:otherwise>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@name"/></xsl:call-template>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@primitiveType"/></xsl:call-template>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@length"/></xsl:call-template>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@presence"/></xsl:call-template>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@minValue"/></xsl:call-template>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@maxValue"/></xsl:call-template>
				<xsl:call-template name="t_type"><xsl:with-param name="value" select="@nullValue"/></xsl:call-template>
			</xsl:otherwise>
		</xsl:choose>
        </tr>
    </xsl:template>
    
    <xsl:template name="t_type">
        <xsl:param name="value"/>
        <td>
        <xsl:choose>
            <xsl:when test="not($value)">
                <span data-tooltip="SBE default">¤</span>
            </xsl:when>
            <xsl:when test="$value='constant'">
                <span data-tooltip="constant"><xsl:value-of select="text()"/></span>
            </xsl:when>
            <xsl:otherwise>
                <xsl:variable name="neat_value">
                    <xsl:call-template name="t_neat_value"><xsl:with-param name="value" select="$value"/></xsl:call-template>
                </xsl:variable>
                <span>
                <xsl:if test="$neat_value!=$value">
                    <xsl:attribute name="data-tooltip">
                        <xsl:value-of select="$value"/>
                    </xsl:attribute>    
                </xsl:if>
                <xsl:copy-of select="$neat_value"/></span>
            </xsl:otherwise>
        </xsl:choose>
        </td>
    </xsl:template>
    
    <xsl:template name="t_neat_value">
        <xsl:param name="value"/>
        <xsl:choose>
		
			<xsl:when test="$value='-9999999999999999'"><xsl:text>-10</xsl:text><sup>16</sup><xsl:text> + 1</xsl:text></xsl:when>
			<xsl:when test="$value='9999999999999999'"><xsl:text>10</xsl:text><sup>16</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='18446744073709551615'"><xsl:text>2</xsl:text><sup>64</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='18446744073709551614'"><xsl:text>2</xsl:text><sup>64</sup><xsl:text> − 2</xsl:text></xsl:when>
            <xsl:when test="$value='-9223372036854775808'"><xsl:text>-2</xsl:text><sup>63</sup></xsl:when>
			<xsl:when test="$value='9223372036854775807'"><xsl:text>2</xsl:text><sup>63</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='9223372036854775806'"><xsl:text>2</xsl:text><sup>63</sup><xsl:text> − 2</xsl:text></xsl:when>
			<xsl:when test="$value='4294967295'"><xsl:text>2</xsl:text><sup>32</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='4294967294'"><xsl:text>2</xsl:text><sup>32</sup><xsl:text> − 2</xsl:text></xsl:when>
			<xsl:when test="$value='-2147483648'"><xsl:text>-2</xsl:text><sup>31</sup><xsl:text></xsl:text></xsl:when>
			<xsl:when test="$value='2147483647'"><xsl:text>2</xsl:text><sup>31</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='2147483646'"><xsl:text>2</xsl:text><sup>31</sup><xsl:text> − 2</xsl:text></xsl:when>
		    <xsl:when test="$value='65535'"><xsl:text>2</xsl:text><sup>16</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='65534'"><xsl:text>2</xsl:text><sup>16</sup><xsl:text> − 2</xsl:text></xsl:when>
			<xsl:when test="$value='-32768'"><xsl:text>-2</xsl:text><sup>15</sup><xsl:text></xsl:text></xsl:when>
			<xsl:when test="$value='32767'"><xsl:text>2</xsl:text><sup>15</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='32766'"><xsl:text>2</xsl:text><sup>15</sup><xsl:text> − 2</xsl:text></xsl:when>
			<xsl:when test="$value='255'"><xsl:text>2</xsl:text><sup>8</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='254'"><xsl:text>2</xsl:text><sup>8</sup><xsl:text> − 2</xsl:text></xsl:when>
			<xsl:when test="$value='-128'"><xsl:text>-2</xsl:text><sup>7</sup><xsl:text></xsl:text></xsl:when>
			<xsl:when test="$value='127'"><xsl:text>2</xsl:text><sup>7</sup><xsl:text> − 1</xsl:text></xsl:when>
			<xsl:when test="$value='126'"><xsl:text>2</xsl:text><sup>7</sup><xsl:text> − 2</xsl:text></xsl:when>
            <xsl:otherwise><xsl:value-of select="$value"/></xsl:otherwise>
        </xsl:choose>
    </xsl:template>
    
    <xsl:template name="t_css">
        <style type="text/css">



:root 
{
    font: 11pt sans-serif;
    --color-bg: #FFFFFF;
    --color-text: #000000;
    --color-link: #000000;
    --color-accent: #ce1126;
    --color-accent-bg: #f5f5f5;
    --color-table-header: grey;
    --color-border-light: lightgrey;
    --color-border-dark: grey;
    --font-title: 2rem;
    --font-version: .8rem;
    --font-section-header: 1.4rem;
    --font-entity-header: 1.1rem;
    --font-table-header: 0.9rem;
    --font-normal: 0.9rem;
}

* 
{
    box-sizing: border-box;
}

body 
{
    width: 60rem;
    margin: 1rem;
    text-align: left;
    line-height: 1.4;
    background: var(--color-bg);
    color: var(--color-text);
}   

:link, 
:visited 
{
    color: var(--color-link);
    text-decoration: underline solid var(--color-link) 1px;
 
}

::marker
{
    color: var(--color-accent);
}           


.schema-title, 
.schema-title:before 
{
    font-size: var(--font-title);
    font-weight: bold;
}

.schema-title:before 
{
    content:url("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAAACXBIWXMAAC4jAAAuIwF4pT92AAABJUlEQVR42mM4IaD3n5aYYdSCEW7BZeeo/2fVHHHKn9Vw/n/ZMZJ0C06KGf9/s2bb/3///v3/duMOVktAhn+7df//v79//79eteX/SVEj4i04o2T7HxmgWwIzHBmclrcm3YJ/v37//373IZj95eL1/6cVbFAMB/vgzx/yLfj7/cf/c1qu/79eu/3/04lz/x+3TUMxHGTZ35+/KLMAzFex//+kc8b/X6/eoBgOkqOKBeBguQ1x+a+Xb/4/bp0CDi6qWIAS5kBLHndM///x2Nn/Xy5cA0c8ZRYAIxk9zEHBBYoTWOr695uCSIYnU6QwB2FQxMNSF1nJ9ISw4f9nUxaCkyC64eiW/Pv9+//TifOBegzIKCqAxQA2w5EtuWwfPlpcj1pAYwsASlUoUwKeaMAAAAAASUVORK5CYII=");
    margin-right: 0.5rem;
}

.schema-version 
{
    padding: 0.5rem 0;
    margin-bottom: 4rem;
    font-size: var(--font-version);
    border-bottom: 2px solid var(--color-accent);
}           


.toc_section, 
.content 
{
    margin-top: 2rem;
}
            
.toc_section > a,
.content > span
{
    display: block;
    background: var(--color-accent-bg);
    border-bottom: 2px solid var(--color-accent);
    padding: 1rem .5rem;
    font-size: var(--font-section-header);
}

.toc_section ul 
{
    list-style-type: square;
}


.toc_section li > a 
{
    display: flex;
    width: 100%;    
    font-size: var(--font-normal);
}

.toc_section li > a > span:first-of-type 
{
    width: 70%; 
}

.toc_section li > a > span:last-of-type {
    width: 30%; 
    text-align:right;
    padding: 0 1rem;
}

table 
{
    width: 100%;
    table-layout: fixed;
    empty-cells: show;
    border-collapse: collapse;
    border: 1px solid var(--color-border-light);
    border-spacing: 0;
}

th, td
{
    padding: .1rem .5rem;
	text-align: left;
}

th 
{
    background: var(--color-accent-bg);
    color: var(--color-table-header);
    font-size: var(--font-table-header);
    font-weight: normal;
    white-space: nowrap;
}


td
{
    font-size: var(--font-normal);
}


.entity-name > span
{
    font-size: var(--font-entity-header);
    display: list-item;
    list-style-type: square;
}

.entity-anchor {
    margin: 1.5rem 0 0 0;
    height: .5rem;
}

.messages span
{
    padding-left: .1rem ; 
}

.x-indent 
{
    padding: 0rem 0 0rem .2rem;
    margin: 0px 0 0rem .8rem;
    border-left: 1px dotted grey;
}

:is(.messages, .enums, .sets) tr > :is(th, td):nth-of-type(1)
{
    padding-left: 1.5rem;
    width: max-content;
}

:is(.messages, .enums, .sets) tr > :is(th, td):nth-of-type(2)
{
    width: 35%;
    text-align: right;
}


:is(.messages, .enums, .sets) tr > :is(th, td):nth-of-type(3)
{
    width: 30%;
    text-align: right;
}
 


:is(.composites, .types) tr > :is(th, td) +:not(a)
{
    text-align: right;
}


:is(.composites, .types) tr > :is(th, td):nth-of-type(1)
{
    padding-left: 1.5rem;
    width: 22%;
    text-align: left;
}

:is(.composites, .types) tr > :is(th, td):nth-of-type(2)
{
    min-width: 30%;
}



.types tr > td:first-of-type span
{
    display: list-item;
    list-style-type: square;
}

        
*[data-tooltip]
{
    text-decoration: underline;
    text-decoration-style: dotted;
    text-decoration-color: grey;
    text-decoration-thickness: 1px;
}


[data-tooltip]:after 
{            
    position: absolute;
    content: attr(data-tooltip);
    opacity: 0;
    background: white;
    border: 1px solid grey;
    border-radius: 4px;
    padding: 0.1rem 0.4rem;
    transform: translateX(-0.2rem) translateY(-1.2rem);
    color: black;
    font-size: 0.8rem;
}


[data-tooltip]:hover:after 
{        
    opacity: 1;
}        

        
[data-tooltip]:hover 
{        
    color: red;
    cursor:  context-menu;
}        


.toc_section li:hover,
tr:hover :not(th)
{
    background-color: #fff4f4;
}

      
.footer 
{
	font-size: .7rem;
	margin-top: 4rem;
}	  


:target +table,
tr:target
{
  animation: blink .5s;
  animation-iteration-count: 4;
}


@keyframes blink 
{ 
    100% {outline: 1px solid var(--color-accent);}  
}
        
        </style>
    </xsl:template>                 

</xsl:stylesheet>
